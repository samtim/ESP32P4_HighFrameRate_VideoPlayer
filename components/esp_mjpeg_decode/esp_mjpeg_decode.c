#include "esp_mjpeg_decode.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mjpeg_decode";

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

    if (!mjpeg->mjpeg_buf || !mjpeg->output_buf) {
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
    return true;
}

bool esp_mjpeg_decode_read_frame(esp_mjpeg_decode_t *mjpeg) {
    if (!mjpeg->file) return false;

    int c;
    while ((c = fgetc(mjpeg->file)) != EOF) {
        if (c == 0xFF) {
            int c2 = fgetc(mjpeg->file);
            if (c2 == 0xD8) {
                mjpeg->mjpeg_buf[0] = 0xFF;
                mjpeg->mjpeg_buf[1] = 0xD8;
                break;
            }
        }
    }

    if (feof(mjpeg->file)) return false;

    size_t idx = 2;
    while ((c = fgetc(mjpeg->file)) != EOF) {
        if (idx >= mjpeg->mjpeg_buf_alloc_size) {
            ESP_LOGE(TAG, "JPEG Frame size exceeded buffer size!");
            return false;
        }
        mjpeg->mjpeg_buf[idx++] = (uint8_t)c;
        if (mjpeg->mjpeg_buf[idx - 2] == 0xFF && mjpeg->mjpeg_buf[idx - 1] == 0xD9) {
            mjpeg->mjpeg_buf_size = idx;
            return true;
        }
    }
    return false;
}

esp_err_t esp_mjpeg_decode_frame(esp_mjpeg_decode_t *mjpeg) {
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    
    uint32_t out_size = 0;
    return jpeg_decoder_process(mjpeg->decoder_engine, &decode_cfg, 
                                mjpeg->mjpeg_buf, mjpeg->mjpeg_buf_size, 
                                mjpeg->output_buf, mjpeg->output_buf_alloc_size, 
                                &out_size);
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
    if (mjpeg->decoder_engine) jpeg_del_decoder_engine(mjpeg->decoder_engine);
}
