/**
 ******************************************************************************
 * @file        app_display.h
 * @brief       屏幕显示层: 把解码后的一帧刷到屏上
 *
 * 从 main.c 抽出来, 让播放器只关心"解出帧", 不关心"怎么画"。
 ******************************************************************************
 */
#ifndef __APP_DISPLAY_H_
#define __APP_DISPLAY_H_

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

/** @brief 传入已初始化的 MIPI-DSI 面板句柄 */
void app_display_init(esp_lcd_panel_handle_t panel);

/**
 * @brief  把一帧 RGB888 画面刷到屏幕
 * @param  fb   帧缓冲 (RGB888, 行跨距 = w*3)
 * @param  w,h  画面宽高
 * @return ESP_OK 已提交; 其它值表示这次没刷上去(上层可计数)
 */
esp_err_t app_display_frame(uint8_t *fb, int w, int h);

/**
 * @brief 直接把一块 RGB888 缓冲刷到屏幕
 * @note  内部对 DMA2D 忙状态做了短重试(1ms 一步)
 */
esp_err_t app_display_blit(uint8_t *fb, int w, int h);

/** @brief 取刷屏统计: retries=DMA2D 忙而重试过的帧数, drops=最终没刷上去的帧数 */
void app_display_get_stats(uint32_t *retries, uint32_t *drops);

/** @brief 清零刷屏统计 */
void app_display_clear_stats(void);

#endif /* __APP_DISPLAY_H_ */
