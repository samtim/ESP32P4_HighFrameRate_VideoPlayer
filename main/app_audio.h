/**
 ******************************************************************************
 * @file        app_audio.h
 * @brief       音频播放层 (WAV / MP3 -> PSRAM 环形缓冲 -> I2S)
 *
 * 用法:
 *     app_audio_play("/sdcard/600L.mjpeg");   // 自动去找同名的 .wav / .mp3
 *     ...
 *     int64_t pts = app_audio_get_clock_us(); // 音频主时钟, 用于 A/V 同步
 *     ...
 *     app_audio_stop();
 *
 * 支持格式:
 *   .wav  16bit PCM, 单声道或立体声 (任意采样率, 8k~96k)
 *   .mp3  MPEG-1/2 Layer III (minimp3 软解, 任意常见采样率/码率, 单声道或立体声)
 *   查找顺序: 同名的 .wav 优先(无损), 没有则用 .mp3; 实际格式以文件内容判定。
 *
 * 设计说明:
 *   播放分两级缓冲, 解决"边播边读 SD/解码就会断续"的问题:
 *     音频源任务(低优先级, core 1) --读文件+MP3解码--> PSRAM 环形缓冲(约 1.5s)
 *        --> 播放任务(高优先级, core 0) --> I2S DMA
 *   播放任务只从内存搬数据, 不受 SD 读写延迟和视频解码抢占的影响;
 *   MP3 解码放在 core 1, 不与 core 0 上的视频解码抢 CPU。
 ******************************************************************************
 */
#ifndef __APP_AUDIO_H_
#define __APP_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief  开始播放 "<media_path 去掉扩展名>.wav"
 * @param  media_path 视频文件路径, 函数内部只替换扩展名为 .wav
 * @note   立即返回, 播放由内部任务完成; 找不到 WAV 时会打警告并让视频改用系统定时器同步
 */
esp_err_t app_audio_play(const char *media_path);

/** @brief 请求停止播放 (内部任务会在 100ms 内自行退出) */
void app_audio_stop(void);

/**
 * @brief  音频主时钟 (微秒)
 * @return 播放开始前返回 0, 播放开始后返回自起播以来的微秒数
 * @note   找不到/无法解析 WAV 时, 也会在起播时刻返回非 0 (用系统定时器兜底), 保证视频照常同步
 */
int64_t app_audio_get_clock_us(void);

/** @brief 是否正在播放 */
bool app_audio_is_playing(void);

/**
 * @brief  暂停 / 继续播放
 * @note   暂停期间音频任务不写 I2S, 同时冻结音频主时钟(恢复时把暂停时长补回去),
 *         这样音视频同步不会因为暂停而错乱。调用方应同时暂停视频解码。
 */
void app_audio_set_paused(bool paused);

/** @brief 是否处于暂停状态 */
bool app_audio_is_paused(void);

#endif /* __APP_AUDIO_H_ */
