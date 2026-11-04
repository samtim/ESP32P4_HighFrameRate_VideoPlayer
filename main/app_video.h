/**
 ******************************************************************************
 * @file        app_video.h
 * @brief       MJPEG 视频播放 (SD 流式读取 + 硬件 JPEG 解码 + A/V 同步 + 按键)
 *
 * 用法:
 *     app_video_init();                       // 建硬件 JPEG 解码器一次
 *     app_video_result_t r = app_video_play(path);  // 播一个文件, 返回结束原因
 ******************************************************************************
 */
#ifndef __APP_VIDEO_H_
#define __APP_VIDEO_H_

#include "esp_err.h"

/** @brief 播放结束原因 */
typedef enum {
    APP_VIDEO_DONE = 0,         /* 正常播完 */
    APP_VIDEO_NEXT,             /* 用户按了"下一首" */
    APP_VIDEO_PREV,             /* 用户按了"上一首" */
} app_video_result_t;

/** @brief 创建硬件 JPEG 解码引擎 (开机调一次) */
esp_err_t app_video_init(void);

/**
 * @brief  播放一个 MJPEG 文件(音频按同名 .wav/.mp3 自动配对)
 * @param  path  视频路径
 * @return 结束原因, 调用方据此决定下一个播哪个文件
 */
app_video_result_t app_video_play(const char *path);

#endif /* __APP_VIDEO_H_ */
