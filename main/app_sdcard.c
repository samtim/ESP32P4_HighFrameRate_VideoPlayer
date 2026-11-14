#include "app_sdcard.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "app_sdcard";
static sdmmc_card_t *s_card = NULL;

#define SD_PWR_EN_GPIO      GPIO_NUM_45 
#define SD_PHY_LDO_CHAN     4           
#define SD_PHY_LDO_VOLT_MV  3300        

/* 冷启动时卡上电比较慢, 第一次挂载偶尔会超时(0x107)。
 * 这里给足稳定时间, 并且失败就断电重来, 最多 5 次; main 里还会再循环重试,
 * 所以 SD 卡慢一点也不至于 panic 重启。 */
#define SD_MOUNT_ATTEMPTS   5
#define SD_POWER_SETTLE_MS  300     /* 上电后等多久再发第一条命令 */
#define SD_POWER_OFF_MS     100     /* 重试前断电多久 */

static bool s_power_inited = false;

/* GPIO45 低电平 = 给卡供电(本板实测)。重试时先拉高断电, 让卡彻底复位再上电 */
static void sd_power_cycle(bool first)
{
    if (!first) {
        gpio_set_level(SD_PWR_EN_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(SD_POWER_OFF_MS));
    }
    gpio_set_level(SD_PWR_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_SETTLE_MS));
}

esp_err_t app_sdcard_init(void) {
    if (!s_power_inited) {
        ESP_LOGI(TAG, "Enabling SD Card PHY LDO4 Power (3.3V)...");

        esp_ldo_channel_handle_t ldo4_handle = NULL;
        esp_ldo_channel_config_t ldo4_cfg = {
            .chan_id = SD_PHY_LDO_CHAN,
            .voltage_mv = SD_PHY_LDO_VOLT_MV,
        };
        esp_ldo_acquire_channel(&ldo4_cfg, &ldo4_handle);

        gpio_config_t pwr_conf = {
            .pin_bit_mask = (1ULL << SD_PWR_EN_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&pwr_conf);
        s_power_inited = true;
    }

    /* 每次尝试都重新构造 host/slot, 因为挂载失败时驱动内部已经 deinit 过了 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // ESP32-P4: SD 卡用 SLOT_0(GPIO39-44); SLOT_1(GPIO49-54) 留给 ESP-Hosted 的 SDIO
    // (参考厂商 07 例程: "For ESP32-P4, only Slot 0 for SD Card is available as ESP-Hosted uses Slot 1" )
    host.slot = SDMMC_HOST_SLOT_0;
    
    // ★ 1. 将 SD 卡传输时钟频率设置为 40MHz
    host.max_freq_khz = 40000; 

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* 内部上拉, 厂商 07 例程同款 */
    slot_config.clk   = GPIO_NUM_43;
    slot_config.cmd   = GPIO_NUM_44;
    slot_config.d0    = GPIO_NUM_39;
    slot_config.d1    = GPIO_NUM_40;
    slot_config.d2    = GPIO_NUM_41;
    slot_config.d3    = GPIO_NUM_42;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        // ★ 2. 将 FATFS 挂载分配块大小设置为 16KB
        .allocation_unit_size = 16 * 1024 
    };

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
        sd_power_cycle(attempt == 1);

        ESP_LOGI(TAG, "初始化 SD Card (SDMMC Slot 0, GPIO39-44, 第 %d/%d 次)...",
                 attempt, SD_MOUNT_ATTEMPTS);

        sdmmc_card_t *card = NULL;
        ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK) {
            s_card = card;
            sdmmc_card_print_info(stdout, card);
            app_sdcard_cleanup_partial();
            return ESP_OK;
        }

        ESP_LOGW(TAG, "SD 卡挂载失败 (0x%x), 断电重试", ret);
    }

    ESP_LOGE(TAG, "SD 卡初始化失败 %d 次 (0x%x)", SD_MOUNT_ATTEMPTS, ret);
    return ret;
}

/* 清理上次没下完的残留: 0 字节文件 + *.part
 * 这些文件会让播放器判断成"有视频但 0 帧", 或者让同步误以为已存在 */
void app_sdcard_cleanup_partial(void)
{
    static const char *dirs[] = { "/sdcard/001", "/sdcard/002", "/sdcard/003", "/sdcard/004", "/sdcard/005" };
    int removed = 0;

    for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) {
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            char full[320];
            snprintf(full, sizeof(full), "%s/%s", dirs[i], e->d_name);

            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }

            size_t n = strlen(e->d_name);
            bool is_part = (n > 5) && (strcmp(e->d_name + n - 5, ".part") == 0);
            if (st.st_size == 0 || is_part) {
                if (unlink(full) == 0) {
                    removed++;
                    ESP_LOGW(TAG, "删除残留文件: %s (%ld 字节)", full, (long)st.st_size);
                }
            }
        }
        closedir(d);
    }

    if (removed > 0) {
        ESP_LOGW(TAG, "共清理 %d 个残留文件", removed);
    }
}

/* 裸扇区读基准: 绕过 FATFS, 直接读 4MB, 判断是"文件系统慢"还是"硬件慢" */
void app_sdcard_raw_bench(void)
{
    static bool done = false;
    if (done || s_card == NULL) {
        return;   /* 只跑一次, 不占用 DMA 内存 */
    }
    done = true;

    const size_t sectors = 256;                     /* 128KB */
    uint8_t *buf = heap_caps_malloc(sectors * 512, MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGW(TAG, "裸读基准: 缓冲分配失败");
        return;
    }

    const int loops = 32;                           /* 32 * 128KB = 4MB */
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    for (int i = 0; i < loops; i++) {
        err = sdmmc_read_sectors(s_card, buf, 4096 + (size_t)i * sectors, sectors);
        if (err != ESP_OK) {
            break;
        }
    }
    int64_t dt = esp_timer_get_time() - t0;
    free(buf);

    if (err == ESP_OK && dt > 0) {
        int kb = loops * 128;
        ESP_LOGI(TAG, "裸扇区读基准: %d KB / %d ms = %d KB/s (绕过 FATFS)",
                 kb, (int)(dt / 1000), (int)((long long)kb * 1024 * 1000000 / (dt * 1024)));
    } else {
        ESP_LOGW(TAG, "裸读基准失败: %s", esp_err_to_name(err));
    }
}
