/**
 ******************************************************************************
 * @file        app_playlist.h
 * @brief       SD 卡视频清单 (.mjpeg / .mjpg), 用于循环播放
 *
 * 用法:
 *     if (app_playlist_scan(&pl, "/sdcard") == ESP_OK) {
 *         for (int i = 0; i < pl.count; i++) {
 *             play(pl.files[i]);      // 音频由播放层按同名 .wav/.mp3 自动配对
 *         }
 *     }
 *
 * 说明:
 *   - 只扫描指定目录(不含子目录)
 *   - 结果按文件名排序, 保证每次播放顺序固定(readdir 本身是乱序的)
 *   - 每轮循环重新扫描一次, 所以播放中途拷入新文件也能被拾取
 ******************************************************************************
 */
#ifndef __APP_PLAYLIST_H_
#define __APP_PLAYLIST_H_

#include <stdbool.h>
#include "esp_err.h"

/* 最多收录多少个视频, 以及单个路径的最大长度(含结尾 0) */
#define APP_PLAYLIST_MAX        24
#define APP_PLAYLIST_PATH_LEN   256

typedef struct {
    char files[APP_PLAYLIST_MAX][APP_PLAYLIST_PATH_LEN];
    int  count;
} app_playlist_t;

/**
 * @brief  扫描目录下所有 .mjpeg / .mjpg 文件
 * @param  pl  清单对象
 * @param  dir 目录, 例如 "/sdcard"
 * @return ESP_OK 找到至少 1 个; ESP_ERR_NOT_FOUND 目录打不开或没有视频文件
 */
esp_err_t app_playlist_scan(app_playlist_t *pl, const char *dir);

/**
 * @brief  确保目录存在(不存在就创建, 只建一级)
 * @param  dir 例如 "/sdcard/001"
 */
esp_err_t app_playlist_ensure_dir(const char *dir);

#endif /* __APP_PLAYLIST_H_ */
