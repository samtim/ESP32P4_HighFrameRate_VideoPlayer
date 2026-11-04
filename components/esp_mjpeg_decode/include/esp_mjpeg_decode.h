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

#ifdef __cplusplus
}
#endif

#endif // ESP_MJPEG_DECODE_H
