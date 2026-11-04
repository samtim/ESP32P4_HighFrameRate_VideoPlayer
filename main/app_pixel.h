/**
 ******************************************************************************
 * @file        app_pixel.h
 * @brief       RGB888 像素字节序 (全工程唯一约定)
 *
 * 实测结论: 这块 EK79007 复位后工作在 BGR 模式 —— 按 R,G,B 写进内存,
 * 屏上从左到右显示的是 蓝,绿,红 (颜色自检条实测)。
 * 所以内存里低地址字节必须放 B。
 *
 * 解码端(JPEG rgb_order)、颜色自检条、OSD 叠加都必须用同一个值。
 ******************************************************************************
 */
#ifndef __APP_PIXEL_H_
#define __APP_PIXEL_H_

#include <stdint.h>

/* 1 = 内存顺序 B,G,R (本板实测) ; 0 = 内存顺序 R,G,B */
#define LCD_PIXEL_ORDER_BGR 1

/* 往一个 RGB888 像素写"逻辑 RGB"值, 内部按上面的字节序排列 */
static inline void app_pixel_put(uint8_t *p, uint8_t r, uint8_t g, uint8_t b)
{
#if LCD_PIXEL_ORDER_BGR
    p[0] = b;
    p[1] = g;
    p[2] = r;
#else
    p[0] = r;
    p[1] = g;
    p[2] = b;
#endif
}

/* 从 RGB888 像素读回"逻辑 RGB"值 (半透明叠加时需要先读底色) */
static inline void app_pixel_get(const uint8_t *p, uint8_t *r, uint8_t *g, uint8_t *b)
{
#if LCD_PIXEL_ORDER_BGR
    *r = p[2];
    *g = p[1];
    *b = p[0];
#else
    *r = p[0];
    *g = p[1];
    *b = p[2];
#endif
}

#endif /* __APP_PIXEL_H_ */
