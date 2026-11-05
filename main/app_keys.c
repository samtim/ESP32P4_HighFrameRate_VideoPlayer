/**
 ******************************************************************************
 * @file        app_keys.c
 * @brief       5 个按键扫描 (20ms 采样 + 3 次去抖)
 *
 * 极性: 开机自适应。做法是先把脚设成"浮空/内部下拉/内部上拉"各读一次:
 *   - 内部下拉能把脚读到 0  -> 这颗键是"高电平按下", 保持内部下拉;
 *   - 内部下拉拉不下来(读到 1, 说明外部有更强的上拉)
 *                           -> 只能当"低电平按下"用(按键必须对地), 内部上拉;
 *   并且把"当前电平"当作初始状态, 不会在开机时误产生一次按键事件。
 * 需要强制时改 APP_KEYS_POLARITY。
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
#define KEY_SETTLE_MS       20      /* 切换上下拉后等电平稳定 */

typedef struct {
    int gpio;
    app_key_event_t event;
    bool     pressed;       /* 当前稳定状态 */
    bool     active_high;   /* true: 按下为高电平; false: 按下为低电平 */
    int      cnt;           /* 去抖计数 */
} key_ctx_t;

static key_ctx_t s_keys[] = {
    { APP_KEY_PLAY_PAUSE_GPIO, APP_KEY_PLAY_PAUSE, false, true, 0 },
    { APP_KEY_NEXT_GPIO,       APP_KEY_NEXT,       false, true, 0 },
    { APP_KEY_PREV_GPIO,       APP_KEY_PREV,       false, true, 0 },
    { APP_KEY_SPEED_UP_GPIO,   APP_KEY_SPEED_UP,   false, true, 0 },
    { APP_KEY_SPEED_DOWN_GPIO, APP_KEY_SPEED_DOWN, false, true, 0 },
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
    const int n = (int)(sizeof(s_keys) / sizeof(s_keys[0]));

    uint64_t mask = 0;
    for (int i = 0; i < n; i++) {
        mask |= (1ULL << s_keys[i].gpio);
    }

    esp_err_t err = gpio_config(&(gpio_config_t){
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键 GPIO 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 三态测量: 浮空 / 内部下拉 / 内部上拉 各读一次 ---- */
    int lv_float[8], lv_pd[8], lv_pu[8];
    for (int pass = 0; pass < 3; pass++) {
        int *dst = (pass == 0) ? lv_float : (pass == 1) ? lv_pd : lv_pu;
        gpio_pull_mode_t mode = (pass == 0) ? GPIO_FLOATING
                               : (pass == 1) ? GPIO_PULLDOWN_ONLY
                                             : GPIO_PULLUP_ONLY;
        for (int i = 0; i < n; i++) {
            gpio_set_pull_mode(s_keys[i].gpio, mode);
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_SETTLE_MS));
        for (int i = 0; i < n; i++) {
            dst[i] = gpio_get_level(s_keys[i].gpio);
        }
    }

    for (int i = 0; i < n; i++) {
#if APP_KEYS_POLARITY == 1
        bool active_high = true;
#elif APP_KEYS_POLARITY == 2
        bool active_high = false;
#else
        /* 内部下拉能拉低 -> 按下给高电平; 拉不低 -> 外部有强上拉, 只能按低电平按下用 */
        bool active_high = (lv_pd[i] == 0);
#endif
        s_keys[i].active_high = active_high;

        /* 按极性把内部电阻定下来: 高电平有效用下拉, 低电平有效用上拉 */
        esp_err_t e = gpio_set_pull_mode(s_keys[i].gpio,
                                         active_high ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d 设置上下拉失败: %s", s_keys[i].gpio, esp_err_to_name(e));
        }
    }

    /* 等稳定后把当前电平当作初始状态 —— 这样开机不会误发一次按键事件 */
    vTaskDelay(pdMS_TO_TICKS(KEY_SETTLE_MS));
    for (int i = 0; i < n; i++) {
        bool level = gpio_get_level(s_keys[i].gpio) ? true : false;
        s_keys[i].pressed = s_keys[i].active_high ? level : !level;
        s_keys[i].cnt = 0;

        ESP_LOGI(TAG, "GPIO%-2d 浮空=%d 下拉=%d 上拉=%d -> %s, 空闲=%s",
                 s_keys[i].gpio, lv_float[i], lv_pd[i], lv_pu[i],
                 s_keys[i].active_high ? "高电平有效(内部下拉)"
                                       : "低电平有效(内部上拉, 按键须对地)",
                 s_keys[i].pressed ? "按下" : "未按下");

        if (s_keys[i].pressed) {
            ESP_LOGW(TAG, "GPIO%d 开机就处于按下态: 按键接线/外部上下拉有问题",
                     s_keys[i].gpio);
        }
        if (lv_pd[i] != 0 && lv_pu[i] != 1) {
            ESP_LOGW(TAG, "GPIO%d 内部上下拉都改不动电平 -> 外部有强驱动, 这颗键不可靠",
                     s_keys[i].gpio);
        }
    }

    if (xTaskCreate(keys_scan_task, "keys", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建按键任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "按键就绪: GPIO%d=播放/暂停 GPIO%d=下一首 GPIO%d=上一首 GPIO%d=加速 GPIO%d=减速",
             APP_KEY_PLAY_PAUSE_GPIO, APP_KEY_NEXT_GPIO, APP_KEY_PREV_GPIO,
             APP_KEY_SPEED_UP_GPIO, APP_KEY_SPEED_DOWN_GPIO);

    return ESP_OK;
}

app_key_event_t app_keys_take_event(void)
{
    app_key_event_t e = s_event;
    s_event = APP_KEY_NONE;
    return e;
}
