#include "esp_mjpeg_decode.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "mjpeg_decode";

/* 大块读缓存大小. 每帧 JPEG 通常 30~200KB, 32KB 一块可以显著减少
 * VFS/FATFS 的调用次数(原来是逐字节 fgetc, 读速只有 ~1MB/s). */
#define MJPEG_READ_CHUNK  (32 * 1024)

esp_err_t esp_mjpeg_decode_init(esp_mjpeg_decode_t *mjpeg, uint16_t width, uint16_t height) {
    memset(mjpeg, 0, sizeof(esp_mjpeg_decode_t));
    mjpeg->width = width;
    mjpeg->height = height;

    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &mjpeg->decoder_engine));

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    mjpeg->mjpeg_buf_size = width * height;
    mjpeg->mjpeg_buf = jpeg_alloc_decoder_mem(mjpeg->mjpeg_buf_size, &rx_mem_cfg, &mjpeg->mjpeg_buf_alloc_size);

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    mjpeg->output_buf_size = width * height * 3;
    mjpeg->output_buf = jpeg_alloc_decoder_mem(mjpeg->output_buf_size, &tx_mem_cfg, &mjpeg->output_buf_alloc_size);

    /* 读缓存放内部 RAM(SD 读出来的数据先落到这里) */
    mjpeg->rd_buf = heap_caps_malloc(MJPEG_READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mjpeg->rd_buf) {
        mjpeg->rd_buf = malloc(MJPEG_READ_CHUNK);
    }

    if (!mjpeg->mjpeg_buf || !mjpeg->output_buf || !mjpeg->rd_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG hardware DMA buffers");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool esp_mjpeg_decode_open(esp_mjpeg_decode_t *mjpeg, const char *filepath) {
    mjpeg->file = fopen(filepath, "rb");
    if (!mjpeg->file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return false;
    }
    /* 关掉 stdio 的额外缓冲: 我们自己已经用 32KB 大块读了,
     * 让 fread 直接落到 FATFS, 少一次 1KB 级别的搬运 */
    setvbuf(mjpeg->file, NULL, _IONBF, 0);

    mjpeg->rd_pos = 0;
    mjpeg->rd_len = 0;
    return true;
}

/* 把未消费的尾巴挪到缓冲区头部, 再补读新数据; 返回本次新读到的字节数 */
static size_t mjpeg_refill(esp_mjpeg_decode_t *mjpeg) {
    size_t rem = mjpeg->rd_len - mjpeg->rd_pos;
    if (rem && mjpeg->rd_pos) {
        memmove(mjpeg->rd_buf, mjpeg->rd_buf + mjpeg->rd_pos, rem);
    }
    mjpeg->rd_pos = 0;

    size_t got = 0;
    if (rem < MJPEG_READ_CHUNK) {
        got = fread(mjpeg->rd_buf + rem, 1, MJPEG_READ_CHUNK - rem, mjpeg->file);
    }
    mjpeg->rd_len = rem + got;
    return got;
}

/* 在 [p, p+avail) 中查找 marker(0xFF + code), 返回 marker 之后的位置, 找不到返回 NULL */
static const uint8_t *find_marker(const uint8_t *p, size_t avail, uint8_t code) {
    size_t i = 0;
    while (i + 1 < avail) {
        const uint8_t *ff = memchr(p + i, 0xFF, avail - 1 - i);
        if (!ff) {
            break;
        }
        size_t off = (size_t)(ff - p);
        if (p[off + 1] == code) {
            return p + off + 2;
        }
        i = off + 1;
    }
    return NULL;
}

bool esp_mjpeg_decode_read_frame(esp_mjpeg_decode_t *mjpeg) {
    if (!mjpeg->file || !mjpeg->rd_buf) {
        return false;
    }

    uint8_t *dst = mjpeg->mjpeg_buf;
    const size_t cap = mjpeg->mjpeg_buf_alloc_size;
    size_t idx = 0;

    /* 1) 找 SOI (0xFF 0xD8) */
    for (;;) {
        size_t avail = mjpeg->rd_len - mjpeg->rd_pos;
        if (avail < 2) {
            if (mjpeg_refill(mjpeg) == 0) {
                return false;                 /* 文件读完 */
            }
            continue;
        }
        const uint8_t *p = mjpeg->rd_buf + mjpeg->rd_pos;
        const uint8_t *after = find_marker(p, avail, 0xD8);
        if (after) {
            mjpeg->rd_pos += (size_t)(after - p);
            break;
        }
        /* 没找到: 丢掉已扫描的数据, 但保留最后一个字节(可能是 0xFF, 与下一块拼成 marker) */
        size_t keep = (p[avail - 1] == 0xFF) ? 1 : 0;
        mjpeg->rd_pos += avail - keep;
        if (mjpeg_refill(mjpeg) == 0) {
            return false;
        }
    }

    dst[0] = 0xFF;
    dst[1] = 0xD8;
    idx = 2;

    /* 2) 拷贝数据直到 EOI (0xFF 0xD9) */
    for (;;) {
        size_t avail = mjpeg->rd_len - mjpeg->rd_pos;
        if (avail < 2) {
            if (mjpeg_refill(mjpeg) == 0) {
                return false;                 /* 文件被截断 */
            }
            continue;
        }
        const uint8_t *p = mjpeg->rd_buf + mjpeg->rd_pos;
        const uint8_t *after = find_marker(p, avail, 0xD9);
        size_t n;
        if (after) {
            n = (size_t)(after - p);          /* 含 EOI 两个字节 */
        } else {
            n = avail;
            /* 末尾的 0xFF 可能是 EOI 的前半, 留到下一块再判断 */
            if (p[avail - 1] == 0xFF) {
                n = avail - 1;
            }
        }
        if (idx + n > cap) {
            ESP_LOGE(TAG, "JPEG frame size exceeded buffer size! (%u > %u)",
                     (unsigned)(idx + n), (unsigned)cap);
            return false;
        }
        memcpy(dst + idx, p, n);
        idx += n;
        mjpeg->rd_pos += n;

        if (after) {
            mjpeg->mjpeg_buf_size = idx;
            return true;
        }
        if (mjpeg_refill(mjpeg) == 0) {
            return false;
        }
    }
}

esp_err_t esp_mjpeg_decode_frame_to(esp_mjpeg_decode_t *mjpeg, uint8_t *out_buf, size_t out_size, size_t *out_len) {
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t out = 0;
    esp_err_t ret = jpeg_decoder_process(mjpeg->decoder_engine, &decode_cfg,
                                        mjpeg->mjpeg_buf, mjpeg->mjpeg_buf_size,
                                        out_buf, out_size,
                                        &out);
    if (ret == ESP_OK && out_len) {
        *out_len = out;
    }
    return ret;
}

esp_err_t esp_mjpeg_decode_frame(esp_mjpeg_decode_t *mjpeg) {
    return esp_mjpeg_decode_frame_to(mjpeg, mjpeg->output_buf, mjpeg->output_buf_alloc_size, NULL);
}

uint8_t* esp_mjpeg_decode_get_out_buf(esp_mjpeg_decode_t *mjpeg) {
    return mjpeg->output_buf;
}

void esp_mjpeg_decode_close(esp_mjpeg_decode_t *mjpeg) {
    if (mjpeg->file) {
        fclose(mjpeg->file);
        mjpeg->file = NULL;
    }
}

void esp_mjpeg_decode_deinit(esp_mjpeg_decode_t *mjpeg) {
    esp_mjpeg_decode_close(mjpeg);
    if (mjpeg->mjpeg_buf) free(mjpeg->mjpeg_buf);
    if (mjpeg->output_buf) free(mjpeg->output_buf);
    if (mjpeg->rd_buf) free(mjpeg->rd_buf);
    if (mjpeg->decoder_engine) jpeg_del_decoder_engine(mjpeg->decoder_engine);
}
