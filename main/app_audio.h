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
 * @brief  音频主时钟 (媒体时间, 微秒)
 * @return 播放开始前返回 0; 播放开始后返回"已送进 I2S 的音频对应的媒体位置"
 * @note   这个值是**闭环**的: 每送出一块 PCM 就用真实消耗的样点数校正一次,
 *         并且按当前倍速推进, 所以变速后不会积累漂移。
 *         找不到音频文件时用系统定时器兜底(同样按倍速推进), 保证视频照常同步。
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

/* ============================================================================
 * 播放速度 (变速不变调, 用 esp_ae_sonic)
 *   档位 1..5  ->  50% / 75% / 100% / 125% / 150%
 *   开机默认第 3 档(100%); 档位在整机运行期间保持, 换文件不重置
 * ========================================================================== */
#define APP_AUDIO_SPEED_STEP_MIN        1
#define APP_AUDIO_SPEED_STEP_MAX        5
#define APP_AUDIO_SPEED_STEP_DEFAULT    3

/**
 * @brief  设置播放档位 (自动裁剪到 1..5)
 * @note   立即生效: 音频走 sonic 变速不变调, 音频主时钟按新倍率累加,
 *         视频端用同一个时钟做 A/V 同步, 所以音视频同时加/减档且不会失步
 */
void app_audio_set_speed_step(int step);

/** @brief 当前档位 (1..5) */
int app_audio_get_speed_step(void);

/** @brief 当前倍率 (0.5 / 0.75 / 1.0 / 1.25 / 1.5) */
float app_audio_get_speed_factor(void);

#endif /* __APP_AUDIO_H_ */
