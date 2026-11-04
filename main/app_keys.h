/**
 ******************************************************************************
 * @file        app_keys.h
 * @brief       3 个按键 (短按高电平有效)
 *
 *   GPIO8  -> 播放 / 暂停
 *   GPIO27 -> 下一首
 *   GPIO26 -> 上一首
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

typedef enum {
    APP_KEY_NONE = 0,
    APP_KEY_PLAY_PAUSE,
    APP_KEY_NEXT,
    APP_KEY_PREV,
} app_key_event_t;

/** @brief 配置引脚并启动扫描任务 */
esp_err_t app_keys_start(void);

/** @brief 取一个按键事件(取走即清空), 没有则返回 APP_KEY_NONE */
app_key_event_t app_keys_take_event(void);

#endif /* __APP_KEYS_H_ */
