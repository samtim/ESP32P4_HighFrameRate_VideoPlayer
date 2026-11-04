/**
 ******************************************************************************
 * @file        app_video.c
 * @brief       MJPEG 视频播放: SD 流式读取 -> 硬件 JPEG 解码 -> 刷屏 + A/V 同步
 *
 * 从 main.c 抽出来的"视频显示"部分; main.c 只负责"播哪个文件"。
 ******************************************************************************
 */

#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "driver/jpeg_decode.h"

#include "app_video.h"
#include "app_lcd.h"
#include "app_sdcard.h"
#include "app_display.h"
#include "app_audio.h"
#include "app_keys.h"
#include "app_pixel.h"

static const char *TAG = "video";

/* ============================================================================
 * 参数
 * ========================================================================== */
#define LCD_PANEL_WIDTH         LCD_H_RES
#define LCD_PANEL_HEIGHT        LCD_V_RES

#define ALIGN_UP(val, align)    (((val) + (align) - 1) & ~((align) - 1))
#define DECODE_WIDTH            ALIGN_UP(LCD_PANEL_WIDTH, 16)   /* 1024 */
#define DECODE_HEIGHT           ALIGN_UP(LCD_PANEL_HEIGHT, 16)  /* 608 */

#define TARGET_FPS              24
#define FRAME_INTERVAL_US       (1000000 / TARGET_FPS)

#define STREAM_RINGBUF_SIZE     (512 * 1024)
#define SD_READ_CHUNK_SIZE      (64 * 1024)
#define FRAME_BUF_SIZE          (512 * 1024)   /* 单帧上限: 太小会撞上超大帧而卡死 */

/* 屏是 BGR 顺序, 解码端必须输出同样的字节序 (见 app_pixel.h) */
#define JPEG_OUT_RGB_ORDER      (LCD_PIXEL_ORDER_BGR ? JPEG_DEC_RGB_ELEMENT_ORDER_BGR \
                                                     : JPEG_DEC_RGB_ELEMENT_ORDER_RGB)
#define JPEG_OUT_CONV_STD       JPEG_YUV_RGB_CONV_STD_BT601

static jpeg_decoder_handle_t s_decoder = NULL;
static RingbufHandle_t       s_ringbuf = NULL;
static volatile bool         s_finished = false;
static volatile bool         s_reader_running = false;
static volatile int          s_skip_dir = 0;
static bool                  s_paused = false;

/* ============================================================================
 * SD 异步读取任务
 * ========================================================================== */
static void stream_reader_task(void *arg)
{
    const char *filename = (const char *)arg;
    FILE *f = fopen(filename, "rb");
    if (f) { setvbuf(f, NULL, _IOFBF, 64 * 1024); }   /* 默认 1KB 缓冲会拖慢 FATFS 读 */
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", filename);
        s_finished = true;
        s_reader_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(SD_READ_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "读缓冲分配失败");
        fclose(f);
        s_finished = true;
        s_reader_running = false;
        vTaskDelete(NULL);
        return;
    }

    int64_t t_read0 = esp_timer_get_time();
    int64_t bytes_read = 0;

    while (!s_finished) {
        size_t n = fread(buf, 1, SD_READ_CHUNK_SIZE, f);
        if (n == 0) {
            break;
        }

        /* 每 10 秒报一次 SD 实际读速: 远低于 ~2MB/s 就会跟不上 24fps */
        bytes_read += (int64_t)n;
        int64_t now_r = esp_timer_get_time();
        if (now_r - t_read0 >= 10000000) {
            ESP_LOGI(TAG, "SD 读速: %d KB/s", (int)(bytes_read * 1000 / (now_r - t_read0)));
            t_read0 = now_r;
            bytes_read = 0;
        }

        size_t off = 0;
        while (off < n && !s_finished) {
            if (xRingbufferSend(s_ringbuf, buf + off, n - off, pdMS_TO_TICKS(50)) == pdTRUE) {
                off = n;
            }
        }
    }

    fclose(f);
    free(buf);
    s_finished = true;
    s_reader_running = false;
    vTaskDelete(NULL);
}

/* ============================================================================
 * 初始化
 * ========================================================================== */
esp_err_t app_video_init(void)
{
    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 40,
    };
    esp_err_t err = jpeg_new_decoder_engine(&cfg, &s_decoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 解码引擎创建失败: %s", esp_err_to_name(err));
    }
    return err;
}

/* ============================================================================
 * SD 顺序读速基准: 直接读 2MB, 测真实上限(不受播放需求限制)
 * 调试用, 默认不调用, 所以标成 unused
 * ========================================================================== */
static void __attribute__((unused)) sd_read_benchmark(const char *path)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    const size_t bench_size = 2 * 1024 * 1024;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        free(buf);
        return;
    }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);

    int64_t t0 = esp_timer_get_time();
    size_t total = 0;
    while (total < bench_size) {
        size_t n = fread(buf, 1, 256 * 1024, f);
        if (n == 0) {
            break;
        }
        total += n;
    }
    int64_t dt = esp_timer_get_time() - t0;
    fclose(f);
    free(buf);

    if (dt > 0 && total > 0) {
        ESP_LOGI(TAG, "SD 顺序读基准: %d KB / %d ms = %d KB/s",
                 (int)(total / 1024), (int)(dt / 1000), (int)((long long)total * 1000000 / (dt * 1024)));
        ESP_LOGI(TAG, "  (24fps 1024x600 MJPEG 大约需要 1000~4000 KB/s)");
    }
}

/* ============================================================================
 * 播放一个文件
 * ========================================================================== */
app_video_result_t app_video_play(const char *path)
{
    s_paused = false;
    s_skip_dir = 0;
    s_finished = false;
    app_audio_set_paused(false);

    /* sd_read_benchmark(path);     调试用: 需要时打开 */
    /* app_sdcard_raw_bench();      调试用: 需要时打开 */

    s_ringbuf = xRingbufferCreateWithCaps(STREAM_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "环形缓冲创建失败");
        return APP_VIDEO_DONE;
    }

    size_t out_size = 0;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint32_t raw_bytes = DECODE_WIDTH * DECODE_HEIGHT * 3;
    uint8_t *out_buf = (uint8_t *)jpeg_alloc_decoder_mem(raw_bytes, &mem_cfg, &out_size);
    uint8_t *frame_buf = (uint8_t *)heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);

    if (!out_buf || !frame_buf) {
        ESP_LOGE(TAG, "帧缓冲分配失败");
        free(frame_buf);
        free(out_buf);
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
        return APP_VIDEO_DONE;
    }

    s_reader_running = true;
    xTaskCreatePinnedToCore(stream_reader_task, "sd_reader", 4096, (void *)path, 5, NULL, 0);
    app_audio_play(path);

    while (app_audio_get_clock_us() == 0 && !s_finished) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order     = JPEG_OUT_RGB_ORDER,
        .conv_std      = JPEG_OUT_CONV_STD,
    };

    uint32_t total_frames = 0, skipped = 0, fps_count = 0;
    int64_t last_fps = esp_timer_get_time();
    size_t frame_pos = 0;
    int64_t pts_offset = 0;            /* 音频/视频时钟基准偏移(坏帧后重对齐用) */
    uint32_t bad_frames = 0;           /* 坏帧(解码失败/超大帧)计数 */
    uint32_t re_syncs = 0;             /* 时基重对齐次数 */
    int empty_cnt = 0;                 /* 连续空读次数 */
    int64_t last_empty_log = 0;
    bool in_frame = false;

    ESP_LOGI(TAG, "开始播放: %s (目标 %d fps)", path, TARGET_FPS);

    while (!s_finished || xRingbufferGetCurFreeSize(s_ringbuf) < STREAM_RINGBUF_SIZE) {
        /* ---- 按键 / 暂停 ---- */
        {
            app_key_event_t ev = app_keys_take_event();
            if (ev == APP_KEY_NEXT) {
                s_skip_dir = 1;
                s_finished = true;
                ESP_LOGI(TAG, "按键: 下一首");
            } else if (ev == APP_KEY_PREV) {
                s_skip_dir = -1;
                s_finished = true;
                ESP_LOGI(TAG, "按键: 上一首");
            } else if (ev == APP_KEY_PLAY_PAUSE) {
                s_paused = !s_paused;
                app_audio_set_paused(s_paused);
                ESP_LOGI(TAG, "按键: %s", s_paused ? "暂停" : "继续");
            }

            if (s_paused) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        size_t item_size = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_ringbuf, &item_size, pdMS_TO_TICKS(10), 4096);
        if (!data) {
            /* 空读 = 环形缓冲没数据; 连续空读说明 SD 读流跟不上或读任务卡住 */
            empty_cnt++;
            if (empty_cnt >= 100) {
                int64_t now2 = esp_timer_get_time();
                if (now2 - last_empty_log >= 2000000) {
                    last_empty_log = now2;
                    ESP_LOGW(TAG, "读流停顿: 连续空读 %d 次, 已播 %" PRIu32 " 帧, 丢 %" PRIu32 " 帧",
                             empty_cnt, total_frames, skipped);
                }
                empty_cnt = 0;
            }
            continue;
        }
        empty_cnt = 0;

        for (size_t i = 0; i < item_size; i++) {
            uint8_t b = data[i];

            if (!in_frame) {
                if (frame_pos > 0 && frame_buf[frame_pos - 1] == 0xFF && b == 0xD8) {
                    in_frame = true;
                    frame_buf[0] = 0xFF;
                    frame_buf[1] = 0xD8;
                    frame_pos = 2;
                } else {
                    frame_buf[0] = b;
                    frame_pos = 1;
                }
            } else {
                if (frame_pos < FRAME_BUF_SIZE) {
                    frame_buf[frame_pos++] = b;
                } else {
                    /* 单帧超过帧缓冲(损坏帧/超大帧): 丢弃这一帧, 重新找下一个 SOI,
                     * 否则会永远卡在 in_frame 状态, 后面的帧全部丢失 */
                    bad_frames++;
                    ESP_LOGW(TAG, "帧过大(>%d KB), 丢弃并重新同步", FRAME_BUF_SIZE / 1024);
                    in_frame = false;
                    frame_pos = 0;
                    continue;
                }

                if (frame_pos >= 4 && frame_buf[frame_pos - 2] == 0xFF && frame_buf[frame_pos - 1] == 0xD9) {
                    total_frames++;

                    int64_t target_pts = pts_offset + (int64_t)total_frames * FRAME_INTERVAL_US;
                    int64_t audio_pts = app_audio_get_clock_us();
                    int64_t diff = target_pts - audio_pts;

                    if (diff < -FRAME_INTERVAL_US) {
                        skipped++;

                        /* 落后太多 = 读流/解码跟不上(常见于 SD 碎片化)
                         * 不要一直跳帧卡死: 重新对齐时基, 接着播下去 */
                        if (diff < -500000) {
                            pts_offset = audio_pts - (int64_t)total_frames * FRAME_INTERVAL_US;
                            re_syncs++;
                            if ((re_syncs % 10) == 1) {
                                ESP_LOGW(TAG, "播放落后 %.2f 秒, 重新对齐时基 (第 %" PRIu32 " 帧, 累计 %" PRIu32 " 次)",
                                         (double)(-diff) / 1000000.0, total_frames, re_syncs);
                            }
                        }
                    } else {
                        if (diff > 1000) {
                            int64_t sleep_ms = diff / 1000;
                            if (sleep_ms > FRAME_INTERVAL_US / 1000) {
                                sleep_ms = FRAME_INTERVAL_US / 1000;
                            }
                            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
                        }

                        uint32_t actual = 0;
                        if (jpeg_decoder_process(s_decoder, &decode_cfg, frame_buf, (uint32_t)frame_pos,
                                                 out_buf, raw_bytes, &actual) == ESP_OK) {
                            app_display_frame(out_buf, DECODE_WIDTH, LCD_PANEL_HEIGHT);
                            fps_count++;
                        } else {
                            /* 坏帧: 跳过它, 并把时钟基准重新对齐到音频,
                             * 避免后面的帧因为"落后太多"被连续丢弃 */
                            bad_frames++;
                            pts_offset = audio_pts - (int64_t)total_frames * FRAME_INTERVAL_US;
                            if ((bad_frames % 24) == 1) {
                                ESP_LOGW(TAG, "解码失败(第 %" PRIu32 " 帧, %u 字节), 已跳过并重新对齐时钟",
                                         total_frames, (unsigned)frame_pos);
                            }
                        }
                    }

                    int64_t now = esp_timer_get_time();
                    if (now - last_fps >= 10000000) {   /* 每 10 秒一条, 不显示同步差 */
                        ESP_LOGI(TAG, "FPS: %.2f | 已播 %" PRIu32 " 帧 | 丢 %" PRIu32 " 帧",
                                 (float)fps_count * 1000000.0f / (now - last_fps), fps_count, skipped);
                        fps_count = 0;
                        last_fps = now;
                    }

                    in_frame = false;
                    frame_pos = 0;
                }
            }
        }

        vRingbufferReturnItem(s_ringbuf, (void *)data);
    }

    s_finished = true;
    app_audio_stop();

    for (int i = 0; i < 100 && s_reader_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(frame_buf);
    free(out_buf);
    vRingbufferDelete(s_ringbuf);
    s_ringbuf = NULL;

    ESP_LOGI(TAG, "播放结束: 共 %" PRIu32 " 帧, 丢 %" PRIu32 " 帧", total_frames, skipped);

    if (s_skip_dir > 0) {
        return APP_VIDEO_NEXT;
    }
    if (s_skip_dir < 0) {
        return APP_VIDEO_PREV;
    }
    return APP_VIDEO_DONE;
}
