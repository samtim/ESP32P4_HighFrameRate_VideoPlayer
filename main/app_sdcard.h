#ifndef APP_SDCARD_H
#define APP_SDCARD_H

#include "esp_err.h"

esp_err_t app_sdcard_init(void);

/** 删除 0 字节文件和 *.part 等下载残留 (开机自动调用) */
void app_sdcard_cleanup_partial(void);

/** 裸扇区读速基准 (绕过 FATFS) */
void app_sdcard_raw_bench(void);

#endif
