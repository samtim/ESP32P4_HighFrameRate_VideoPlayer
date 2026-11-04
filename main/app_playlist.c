/**
 ******************************************************************************
 * @file        app_playlist.c
 * @brief       SD 卡视频清单 (.mjpeg / .mjpg)
 *
 * 说明:
 *   - 用 stat() 判断"普通文件", 不依赖 dirent 的 d_type(不同 VFS 行为不一致)
 *   - 不依赖 strlcpy, 便于在 PC 上做单元验证
 ******************************************************************************
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "app_playlist.h"

static const char *TAG = "playlist";

/* 视频必须配同名音频(.wav/.mp3), 两者齐备才允许播放 */
static bool has_audio(const char *video_path)
{
    char base[APP_PLAYLIST_PATH_LEN];
    size_t n = strlen(video_path);
    if (n >= sizeof(base)) {
        return false;
    }
    memcpy(base, video_path, n + 1);

    char *dot = strrchr(base, '.');
    if (dot == NULL) {
        return false;
    }
    size_t head = (size_t)(dot - base);

    static const char *exts[] = { ".wav", ".mp3" };
    for (int i = 0; i < 2; i++) {
        if (head + strlen(exts[i]) + 1 >= sizeof(base)) {
            continue;
        }
        strcpy(base + head, exts[i]);
        struct stat st;
        if (stat(base, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            return true;
        }
    }
    return false;
}
static void path_copy(char *dst, const char *src, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 扩展名判断(大小写不敏感) */
static bool has_video_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    return (strcasecmp(dot, ".mjpeg") == 0) || (strcasecmp(dot, ".mjpg") == 0);
}

/* 按文件名排序(插入排序, 数量很小) */
static void sort_by_name(app_playlist_t *pl)
{
    char tmp[APP_PLAYLIST_PATH_LEN];

    for (int i = 1; i < pl->count; i++) {
        path_copy(tmp, pl->files[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcasecmp(pl->files[j], tmp) > 0) {
            path_copy(pl->files[j + 1], pl->files[j], APP_PLAYLIST_PATH_LEN);
            j--;
        }
        path_copy(pl->files[j + 1], tmp, APP_PLAYLIST_PATH_LEN);
    }
}

esp_err_t app_playlist_scan(app_playlist_t *pl, const char *dir)
{
    if (pl == NULL || dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pl->count = 0;

    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGE(TAG, "打不开目录: %s", dir);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_video_ext(e->d_name)) {
            continue;
        }
        if (pl->count >= APP_PLAYLIST_MAX) {
            ESP_LOGW(TAG, "视频超过 %d 个, 后面的被忽略", APP_PLAYLIST_MAX);
            break;
        }

        char full[APP_PLAYLIST_PATH_LEN];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (n < 0 || n >= (int)sizeof(full)) {
            ESP_LOGW(TAG, "路径太长, 跳过: %s", e->d_name);
            continue;
        }

        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        /* 跳过 0 字节文件: 多半是没下完的残留, 播它只会 0 帧死循环 */
        if (st.st_size == 0) {
            ESP_LOGW(TAG, "跳过空文件: %s", full);
            continue;
        }
        /* 视频和同名音频必须都在, 否则不出声就播放了 */
        if (!has_audio(full)) {
            ESP_LOGW(TAG, "缺少同名音频, 跳过: %s", full);
            continue;
        }

        path_copy(pl->files[pl->count], full, APP_PLAYLIST_PATH_LEN);
        pl->count++;
    }

    closedir(d);

    if (pl->count == 0) {
        /* 下载期间目录里还没文件, 限制成最多 10 秒提示一次, 免得刷屏 */
        static int64_t s_last_warn_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_warn_us > 10000000) {
            s_last_warn_us = now;
            ESP_LOGW(TAG, "%s 下没有 .mjpeg/.mjpg 文件", dir);
        }
        return ESP_ERR_NOT_FOUND;
    }

    sort_by_name(pl);

    ESP_LOGI(TAG, "%s 下找到 %d 个视频:", dir, pl->count);
    for (int i = 0; i < pl->count; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i + 1, pl->files[i]);
    }

    return ESP_OK;
}

esp_err_t app_playlist_ensure_dir(const char *dir)
{
    if (dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (mkdir(dir, 0777) == 0) {
        ESP_LOGI(TAG, "创建目录: %s", dir);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "创建目录失败: %s", dir);
    return ESP_FAIL;
}
