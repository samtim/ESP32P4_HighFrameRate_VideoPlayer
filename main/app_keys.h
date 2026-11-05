/**
 ******************************************************************************
 * @file        app_keys.h
 * @brief       5 个按键 (短按触发, 极性开机自适应)
 *
 *   GPIO8  -> 播放 / 暂停
 *   GPIO27 -> 下一首
 *   GPIO26 -> 上一首
 *   GPIO17 -> 加速 (+1 档, 最高第 5 档)
 *   GPIO19 -> 减速 (-1 档, 最低第 1 档)
 *
 * 用法: app_keys_start(); 然后轮询 app_keys_take_event()
 ******************************************************************************
 */
#ifndef __APP_KEYS_H_
#define __APP_KEYS_H_

#include "esp_err.h"

#define APP_KEY_PLAY_PAUSE_GPIO 8
#define APP_KEY_NEXT_GPIO       27
#define APP_KEY_PREV_GPIO       26
#define APP_KEY_SPEED_UP_GPIO   17
#define APP_KEY_SPEED_DOWN_GPIO 19

/* 按键极性:
 *   0 = 开机自适应(默认): 内部下拉能读到 0 -> 高电平有效; 拉不低 -> 低电平有效
 *   1 = 全部强制高电平有效(按键接 3V3, 内部下拉)
 *   2 = 全部强制低电平有效(按键接 GND, 内部上拉)
 * 注意这两个速度键依赖 APP_AUDIO_SPEED_STEP_* 的档位范围。
 */
#define APP_KEYS_POLARITY       0

typedef enum {
    APP_KEY_NONE = 0,
    APP_KEY_PLAY_PAUSE,
    APP_KEY_NEXT,
    APP_KEY_PREV,
    APP_KEY_SPEED_UP,
    APP_KEY_SPEED_DOWN,
} app_key_event_t;

/** @brief 配置引脚(含极性自适应)并启动扫描任务 */
esp_err_t app_keys_start(void);

/** @brief 取一个按键事件(取走即清空), 没有则返回 APP_KEY_NONE */
app_key_event_t app_keys_take_event(void);

#endif /* __APP_KEYS_H_ */
