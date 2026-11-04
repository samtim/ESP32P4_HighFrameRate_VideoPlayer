#ifndef APP_LCD_H
#define APP_LCD_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "app_pixel.h"

#define LCD_H_RES 1024
#define LCD_V_RES 600

/* 屏的像素字节序(内存顺序)统一由 app_pixel.h 定义, 全工程唯一来源:
 *   LCD_PIXEL_ORDER_BGR = 1 -> 内存顺序 B,G,R (本板 EK79007 实测)
 * 解码端(JPEG rgb_order)、颜色自检条、OSD(电量图标) 都使用同一个值。 */

/* 开机颜色自检条: 1 = 开启(约 2 秒), 0 = 关闭
 * 上半屏三条竖条: 左->右 应为 红 / 绿 / 蓝
 *   若显示成 蓝 / 绿 / 红, 说明 LCD_PIXEL_ORDER_BGR 设反了
 * 下半屏左白右灰: 白色若发粉/发青, 那才是偏色(BT601/BT709 或 gamma), 与通道顺序无关 */
#define LCD_COLOR_SELFTEST 1

esp_lcd_panel_handle_t app_lcd_init(void);

/* 颜色自检: 画 R/G/B 三条竖条 + 白/灰块, 停留约 2 秒 */
void lcd_color_selftest(esp_lcd_panel_handle_t panel);

#endif
