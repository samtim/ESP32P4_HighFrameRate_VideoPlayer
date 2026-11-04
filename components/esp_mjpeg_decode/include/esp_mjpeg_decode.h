#ifndef ESP_MJPEG_DECODE_H
#define ESP_MJPEG_DECODE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/jpeg_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE *file;
    jpeg_decoder_handle_t decoder_engine;
    uint8_t *mjpeg_buf;
    size_t mjpeg_buf_size;
    size_t mjpeg_buf_alloc_size;

    uint8_t *output_buf;
    size_t output_buf_size;
    size_t output_buf_alloc_size;

    /* 大块读缓存: 避免逐字节 fgetc 走 VFS/FATFS, 是提高读帧速度的关键 */
    uint8_t *rd_buf;
    size_t rd_pos;
    size_t rd_len;

    uint16_t width;
    uint16_t height;
} esp_mjpeg_decode_t;

esp_err_t esp_mjpeg_decode_init(esp_mjpeg_decode_t *mjpeg, uint16_t width, uint16_t height);
bool esp_mjpeg_decode_open(esp_mjpeg_decode_t *mjpeg, const char *filepath);
bool esp_mjpeg_decode_read_frame(esp_mjpeg_decode_t *mjpeg);
esp_err_t esp_mjpeg_decode_frame(esp_mjpeg_decode_t *mjpeg);
uint8_t* esp_mjpeg_decode_get_out_buf(esp_mjpeg_decode_t *mjpeg);
void esp_mjpeg_decode_close(esp_mjpeg_decode_t *mjpeg);
void esp_mjpeg_decode_deinit(esp_mjpeg_decode_t *mjpeg);

/**
 * @brief 把当前帧解码到调用者指定的输出缓冲(RGB888)
 *
 * 允许直接解码进 LCD 的帧缓冲, 省掉每帧一次 1.8MB 的整屏拷贝。
 * 输出缓冲必须是 64 字节对齐、DMA 可访问的内存(PSRAM 帧缓冲满足要求)。
 *
 * @param out_buf   输出缓冲地址
 * @param out_size  输出缓冲字节数
 * @param out_len   返回实际解码输出的字节数(可为 NULL)
 */
esp_err_t esp_mjpeg_decode_frame_to(esp_mjpeg_decode_t *mjpeg, uint8_t *out_buf, size_t out_size, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // ESP_MJPEG_DECODE_H
