#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_lcd.h"
#include "app_sdcard.h"
#include "esp_mjpeg_decode.h"

static const char *TAG = "main";

#define LCD_PANEL_WIDTH   1024
#define LCD_PANEL_HEIGHT  600

// 针对 ESP32-P4 硬件 JPEG 的 16 像素对齐 (600 -> 608)
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))
#define DECODE_WIDTH         ALIGN_UP(LCD_PANEL_WIDTH, 16)   // 1024
#define DECODE_HEIGHT        ALIGN_UP(LCD_PANEL_HEIGHT, 16)  // 608

void play_mjpeg_from_sd(esp_lcd_panel_handle_t panel, esp_mjpeg_decode_t *mjpeg, const char *filepath) {
    if (!esp_mjpeg_decode_open(mjpeg, filepath)) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
        return;
    }

    ESP_LOGI(TAG, "Start playing video: %s", filepath);

    uint32_t total_frame_count = 0;
    uint32_t fps_frame_count = 0;
    int64_t last_fps_time_us = esp_timer_get_time();

    while (esp_mjpeg_decode_read_frame(mjpeg)) {
        if (esp_mjpeg_decode_frame(mjpeg) == ESP_OK) {
            esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT, 
                                     esp_mjpeg_decode_get_out_buf(mjpeg));
        } else {
            ESP_LOGW(TAG, "Decode error on frame %" PRIu32, total_frame_count);
        }

        total_frame_count++;
        fps_frame_count++;

        int64_t now = esp_timer_get_time();
        if (now - last_fps_time_us >= 1000000) {
            float real_fps = (float)fps_frame_count * 1000000.0f / (now - last_fps_time_us);
            ESP_LOGI(TAG, "Realtime Performance: %.2f FPS (Total Frames: %" PRIu32 ")", real_fps, total_frame_count);
            
            fps_frame_count = 0;
            last_fps_time_us = now;
        }
    }

    ESP_LOGI(TAG, "Video complete, total frames: %" PRIu32, total_frame_count);
    esp_mjpeg_decode_close(mjpeg);
}

void app_main(void) {
    ESP_ERROR_CHECK(app_sdcard_init());
    esp_lcd_panel_handle_t panel = app_lcd_init();

    esp_mjpeg_decode_t mjpeg;
    // 传入 1024x608，分配 1867776 字节内存，消除解码溢出报错
    ESP_ERROR_CHECK(esp_mjpeg_decode_init(&mjpeg, DECODE_WIDTH, DECODE_HEIGHT));

    while (1) {
        play_mjpeg_from_sd(panel, &mjpeg, "/sdcard/600L.mjpeg");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_mjpeg_decode_deinit(&mjpeg);
}