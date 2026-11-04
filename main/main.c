#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_lcd_mipi_dsi.h"

#include "app_lcd.h"
#include "app_sdcard.h"
#include "esp_mjpeg_decode.h"

static const char *TAG = "main";

#define LCD_PANEL_WIDTH   1024
#define LCD_PANEL_HEIGHT  600

// 针对 ESP32-P4 硬件 JPEG 的 16 像素对齐 (600 -> 608)
// 4:2:0 的 1024x600 JPEG 解码后是 1024x608, 缓冲必须按 608 行申请
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))
#define DECODE_WIDTH         ALIGN_UP(LCD_PANEL_WIDTH, 16)   // 1024
#define DECODE_HEIGHT        ALIGN_UP(LCD_PANEL_HEIGHT, 16)  // 608
#define DECODE_BUF_BYTES     ((size_t)DECODE_WIDTH * DECODE_HEIGHT * 3)

/* 每刷新完一帧由 DPI 驱动回调一次 */
static SemaphoreHandle_t s_vsync_sem;

static bool on_lcd_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &woken);
    }
    return woken == pdTRUE;
}

void play_mjpeg_from_sd(esp_lcd_panel_handle_t panel, esp_mjpeg_decode_t *mjpeg, const char *filepath)
{
    /* 双缓冲流水线:
     *   面板正在用 DMA2D 拷贝 dbuf[cur] 的时候, 硬件 JPEG 解码器写 dbuf[cur^1]
     *   每次都卡在屏幕的刷新节拍上开始拷贝 —— 拷贝是从上往下、比逐行扫描快得多,
     *   所以扫描线永远追不上拷贝进度, 画面不会被撕开
     */
    uint8_t *dbuf[2] = { NULL, NULL };
    uint32_t total_frame_count = 0;
    uint32_t fps_frame_count = 0;
    uint32_t decode_errors = 0;
    int64_t last_fps_time_us;
    int64_t acc_wait = 0, acc_read = 0, acc_decode = 0;
    int cur = 0;
    bool vsync_ok = true;
    int vsync_timeouts = 0;

    for (int i = 0; i < 2; i++) {
        /* 硬件 JPEG 解码走 DMA, 缓冲必须 64 字节对齐(驱动内部要做 cache msync) */
        dbuf[i] = heap_caps_aligned_alloc(64, DECODE_BUF_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (!dbuf[i]) {
            ESP_LOGE(TAG, "Failed to allocate decode buffer #%d (%u bytes)", i, (unsigned)DECODE_BUF_BYTES);
            goto cleanup;
        }
    }

    if (!esp_mjpeg_decode_open(mjpeg, filepath)) {
        ESP_LOGE(TAG, "Failed to open %s", filepath);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Start playing video: %s (双缓冲, 解码缓冲 %ux%u)", filepath, DECODE_WIDTH, DECODE_HEIGHT);

    /* 先把第一帧解出来, 后面每次循环都是"先送显示, 再解下一帧" */
    if (!esp_mjpeg_decode_read_frame(mjpeg)) {
        goto cleanup;
    }
    if (esp_mjpeg_decode_frame_to(mjpeg, dbuf[cur], DECODE_BUF_BYTES, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode the first frame");
        goto cleanup;
    }

    last_fps_time_us = esp_timer_get_time();

    for (;;) {
        int64_t t0 = esp_timer_get_time();

        /* 等一帧开始。若上一轮读帧+解码比一帧还慢, 信号早就到了, 这里不会阻塞,
         * 所以帧率不会被同步拖累, 只是让拷贝对齐到帧起始(不撕裂)。 */
        if (vsync_ok) {
            if (xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
                vsync_timeouts = 0;
            } else if (++vsync_timeouts >= 3) {
                vsync_ok = false;
                ESP_LOGW(TAG, "没收到 LCD 刷新事件, 关闭 vsync 同步(可能出现撕裂)");
            }
        }
        int64_t t1 = esp_timer_get_time();

        /* 送显: 缓冲区不在帧缓冲里, 驱动会用 DMA2D 做异步整屏拷贝 */
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_PANEL_WIDTH, LCD_PANEL_HEIGHT, dbuf[cur]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap failed: 0x%x", err);
        }
        int64_t t2 = esp_timer_get_time();

        /* 解码下一帧到另一块缓冲 */
        if (!esp_mjpeg_decode_read_frame(mjpeg)) {
            break;
        }
        int64_t t3 = esp_timer_get_time();

        err = esp_mjpeg_decode_frame_to(mjpeg, dbuf[cur ^ 1], DECODE_BUF_BYTES, NULL);
        int64_t t4 = esp_timer_get_time();

        acc_wait += t1 - t0;
        acc_read += t3 - t2;
        acc_decode += t4 - t3;

        if (err != ESP_OK) {
            decode_errors++;
            total_frame_count++;
            ESP_LOGW(TAG, "Decode error 0x%x on frame %" PRIu32, err, total_frame_count);
            continue;   /* 不交换缓冲, 保持上一帧画面 */
        }

        cur ^= 1;
        total_frame_count++;
        fps_frame_count++;

        if (t4 - last_fps_time_us >= 1000000) {
            float real_fps = (float)fps_frame_count * 1000000.0f / (float)(t4 - last_fps_time_us);
            float n = (float)fps_frame_count;
            ESP_LOGI(TAG, "%.2f FPS | 等 %.2fms 读 %.2fms 解码 %.2fms | 共 %" PRIu32 " 帧, 解码错误 %" PRIu32,
                     real_fps,
                     (float)acc_wait / 1000.0f / n, (float)acc_read / 1000.0f / n,
                     (float)acc_decode / 1000.0f / n,
                     total_frame_count, decode_errors);

            fps_frame_count = 0;
            acc_wait = acc_read = acc_decode = 0;
            last_fps_time_us = t4;
        }
    }

    ESP_LOGI(TAG, "Video complete, total frames: %" PRIu32, total_frame_count);

cleanup:
    esp_mjpeg_decode_close(mjpeg);
    for (int i = 0; i < 2; i++) {
        if (dbuf[i]) {
            free(dbuf[i]);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(app_sdcard_init());
    esp_lcd_panel_handle_t panel = app_lcd_init();

    s_vsync_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_vsync_sem ? ESP_OK : ESP_ERR_NO_MEM);
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = NULL,
        .on_refresh_done = on_lcd_refresh_done,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL));

    esp_mjpeg_decode_t mjpeg;
    // 传入 1024x608，分配 1867776 字节内存，消除解码溢出报错
    ESP_ERROR_CHECK(esp_mjpeg_decode_init(&mjpeg, DECODE_WIDTH, DECODE_HEIGHT));

    while (1) {
        play_mjpeg_from_sd(panel, &mjpeg, "/sdcard/600L.mjpeg");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_mjpeg_decode_deinit(&mjpeg);
}
