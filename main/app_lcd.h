#ifndef APP_LCD_H
#define APP_LCD_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define LCD_H_RES 1024
#define LCD_V_RES 600

esp_lcd_panel_handle_t app_lcd_init(void);

#endif
