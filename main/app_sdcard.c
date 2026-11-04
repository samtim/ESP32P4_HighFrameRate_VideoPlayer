#include "app_sdcard.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_sdcard";

#define SD_PWR_EN_GPIO      GPIO_NUM_45 
#define SD_PHY_LDO_CHAN     4           
#define SD_PHY_LDO_VOLT_MV  3300        

esp_err_t app_sdcard_init(void) {
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
    gpio_set_level(SD_PWR_EN_GPIO, 0);  
    vTaskDelay(pdMS_TO_TICKS(50));      

    ESP_LOGI(TAG, "Initializing SD Card (SDMMC Slot 1)...");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    
    // ★ 关键优化 1：设置 SD 卡最高频率为 SDMMC_FREQ_HIGHSPEED (40MHz / 80MHz)
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; 

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk   = GPIO_NUM_43;
    slot_config.cmd   = GPIO_NUM_44;
    slot_config.d0    = GPIO_NUM_39;
    slot_config.d1    = GPIO_NUM_40;
    slot_config.d2    = GPIO_NUM_41;
    slot_config.d3    = GPIO_NUM_42;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        // ★ 关键优化 2：增大 FATFS 分配块到 64KB，减少文件寻址打断
        .allocation_unit_size = 64 * 1024 
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (0x%x)", ret);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}