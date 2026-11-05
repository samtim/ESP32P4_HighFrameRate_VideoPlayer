/**
 ******************************************************************************
 * @file        main.c
 * @brief       开机直接播放 SD 卡根目录下的 600L.mjpeg (+ 同名音频)
 *
 * 只保留四个功能:
 *   app_sdcard.c   SD 卡挂载/读写
 *   app_video.c    MJPEG 视频播放(硬件 JPEG 解码 + A/V 同步 + 按键)
 *   app_audio.c    WAV/MP3 音频播放 (app_es8311.c 是 ES8311 + I2S 硬件层)
 *   app_display.c  屏幕显示 (app_lcd.c 是 MIPI-DSI 面板初始化)
 *
 * 音频由播放层按同名规则自动配对: 先找 600L.wav, 没有再找 600L.mp3,
 * 两个都没有就用系统定时器当时钟(无声播放)。
 *
 * 已删除: WiFi(app_net)、NFC(app_nfc/app_rc522)、UI(app_ui/app_prompt)。
 ******************************************************************************
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_lcd.h"
#include "app_sdcard.h"
#include "app_audio.h"
#include "app_es8311.h"
#include "app_display.h"
#include "app_keys.h"
#include "app_video.h"

static const char *TAG = "main";

/* 要播的片源: 视频 + 同名音频都放在 SD 卡根目录 */
#define VIDEO_PATH      "/sdcard/600L.mjpeg"

/* 播完到重播之间的停顿 */
#define REPLAY_DELAY_MS 1000

void app_main(void)
{
    /* 1. SD 卡 */
    ESP_ERROR_CHECK(app_sdcard_init());

    /* 2. 屏幕 */
    app_display_init(app_lcd_init());
    lcd_color_selftest(0);            /* 想看开机彩条就把 0 改成 panel 句柄 */

    /* 3. 音频硬件 */
    ESP_ERROR_CHECK(app_audio_hw_init());
    app_audio_hw_selftest_tone();

    /* 4. 视频解码引擎 + 按键 */
    ESP_ERROR_CHECK(app_video_init());
    app_keys_start();

    ESP_LOGI(TAG, "播放速度: 第 %d 档 (%d%%), GPIO%d 加速 / GPIO%d 减速",
             app_audio_get_speed_step(), (int)(app_audio_get_speed_factor() * 100.0f + 0.5f),
             APP_KEY_SPEED_UP_GPIO, APP_KEY_SPEED_DOWN_GPIO);

    /* 5. 一直循环播放这一个文件 */
    while (1) {
        ESP_LOGI(TAG, "===== 播放 %s =====", VIDEO_PATH);

        app_video_result_t r = app_video_play(VIDEO_PATH);   /* 阻塞到播完 */

        ESP_LOGI(TAG, "%s 播放结束(原因 %d), %d ms 后重播",
                 VIDEO_PATH, (int)r, REPLAY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(REPLAY_DELAY_MS));
    }
}
