/**
 ******************************************************************************
 * @file        app_display.c
 * @brief       屏幕显示层
 *
 * 注意: MIPI-DPI 面板是双缓冲 + DMA2D 拷贝, 上一次拷贝没完成时再调 draw_bitmap
 *       会直接返回 ESP_ERR_INVALID_STATE, 所以这里带重试。
 ******************************************************************************
 */

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_display.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;

/* 面板是双缓冲 + DMA2D: 上一次拷贝还没结束时 draw_bitmap 会返回 INVALID_STATE,
 * 所以这里带重试, 保证每帧都真的刷上去 */
static void display_flush(const uint8_t *fb, int w, int h)
{
    if (s_panel == NULL || fb == NULL) {
        return;
    }
    for (int i = 0; i < 50; i++) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, w, h, (uint8_t *)fb);
        if (err == ESP_OK) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "刷屏失败(多次重试)");
}

void app_display_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    ESP_LOGI(TAG, "显示层就绪");
}

void app_display_frame(uint8_t *fb, int w, int h)
{
    display_flush(fb, w, h);
}

void app_display_blit(uint8_t *fb, int w, int h)
{
    display_flush(fb, w, h);
}
