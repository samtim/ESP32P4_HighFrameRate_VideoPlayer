/**
 ******************************************************************************
 * @file        app_keys.c
 * @brief       3 个按键扫描 (短按高电平有效, 20ms 采样 + 3 次去抖)
 ******************************************************************************
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "app_keys.h"

static const char *TAG = "keys";

#define KEY_SCAN_MS         20
#define KEY_DEBOUNCE_CNT    3

typedef struct {
    int gpio;
    app_key_event_t event;
    bool     pressed;       /* 当前稳定状态 */
    bool     active_high;   /* 本板: 短按高电平有效 */
    int      cnt;           /* 去抖计数 */
} key_ctx_t;

static key_ctx_t s_keys[] = {
    { APP_KEY_PLAY_PAUSE_GPIO, APP_KEY_PLAY_PAUSE, false, true, 0 },
    { APP_KEY_NEXT_GPIO,       APP_KEY_NEXT,       false, true, 0 },
    { APP_KEY_PREV_GPIO,       APP_KEY_PREV,       false, true, 0 },
};

static volatile app_key_event_t s_event = APP_KEY_NONE;

static void keys_scan_task(void *arg)
{
    (void)arg;
    const int n = (int)(sizeof(s_keys) / sizeof(s_keys[0]));

    while (1) {
        for (int i = 0; i < n; i++) {
            bool level = gpio_get_level(s_keys[i].gpio) ? true : false;
            bool down = s_keys[i].active_high ? level : !level;

            if (down != s_keys[i].pressed) {
                /* 状态在变, 计数; 稳定若干次才认 */
                if (++s_keys[i].cnt >= KEY_DEBOUNCE_CNT) {
                    s_keys[i].pressed = down;
                    s_keys[i].cnt = 0;
                    if (down) {                 /* 按下即短按(不区分长按) */
                        s_event = s_keys[i].event;
                        ESP_LOGI(TAG, "按键事件: %d", (int)s_keys[i].event);
                    }
                }
            } else {
                s_keys[i].cnt = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_MS));
    }
}

esp_err_t app_keys_start(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        mask |= (1ULL << s_keys[i].gpio);
    }

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* 高电平有效 -> 默认下拉 */
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键 GPIO 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(keys_scan_task, "keys", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建按键任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "按键就绪: GPIO%d=播放/暂停 GPIO%d=下一首 GPIO%d=上一首 (高电平有效)",
             APP_KEY_PLAY_PAUSE_GPIO, APP_KEY_NEXT_GPIO, APP_KEY_PREV_GPIO);
    return ESP_OK;
}

app_key_event_t app_keys_take_event(void)
{
    app_key_event_t e = s_event;
    s_event = APP_KEY_NONE;
    return e;
}
