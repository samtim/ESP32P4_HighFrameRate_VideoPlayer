/**
 ******************************************************************************
 * @file        app_display.c
 * @brief       屏幕显示层
 *
 * 注意: MIPI-DPI 面板是双缓冲 + DMA2D 拷贝, 上一次拷贝没完成时再调 draw_bitmap
 *       会直接返回 ESP_ERR_INVALID_STATE。这里用 1ms 短重试(不能等太久, 否则
 *       36fps 的高速档会因为这 10ms 的睡眠直接丢帧), 并把重试/失败次数暴露出去。
 ******************************************************************************
 */

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_display.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static volatile uint32_t s_retries = 0;     /* 需要重试的次数(DMA2D 还忙着) */
static volatile uint32_t s_drops   = 0;     /* 重试也没刷上去的次数 */

/* 面板是双缓冲 + DMA2D: 上一次拷贝还没结束时 draw_bitmap 会返回 INVALID_STATE。
 * 1ms 一步重试: 一个 DMA2D 整屏拷贝也就几毫秒, 几十次足够, 又不会因为睡眠吞掉帧时间 */
static esp_err_t display_flush(const uint8_t *fb, int w, int h)
{
    if (s_panel == NULL || fb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 40; i++) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, w, h, (uint8_t *)fb);
        if (err == ESP_OK) {
            if (i > 0) {
                s_retries++;
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    s_drops++;
    return ESP_ERR_INVALID_STATE;
}

void app_display_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    ESP_LOGI(TAG, "显示层就绪");
}

esp_err_t app_display_frame(uint8_t *fb, int w, int h)
{
    return display_flush(fb, w, h);
}

esp_err_t app_display_blit(uint8_t *fb, int w, int h)
{
    return display_flush(fb, w, h);
}

void app_display_get_stats(uint32_t *retries, uint32_t *drops)
{
    if (retries) {
        *retries = s_retries;
    }
    if (drops) {
        *drops = s_drops;
    }
}

void app_display_clear_stats(void)
{
    s_retries = 0;
    s_drops = 0;
}
