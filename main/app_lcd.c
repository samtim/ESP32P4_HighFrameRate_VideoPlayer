#include "app_lcd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "app_lcd";

#define LCD_BL_PIN       GPIO_NUM_23  
#define LCD_RST_PIN      GPIO_NUM_22  
#define LCD_PWREN_PIN    GPIO_NUM_21  

#define EK79007_INIT_CMD_LEN  9

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
} mipi_lcd_init_cmd_t;

static const mipi_lcd_init_cmd_t ek79007_init_cmds[] = {
    {0x80, (uint8_t[]){0x8B}, 1},
    {0x81, (uint8_t[]){0x78}, 1},
    {0x82, (uint8_t[]){0x84}, 1},
    {0x83, (uint8_t[]){0x88}, 1},
    {0x84, (uint8_t[]){0xA8}, 1},
    {0x85, (uint8_t[]){0xE3}, 1},
    {0x86, (uint8_t[]){0x88}, 1},
    {0xB1, (uint8_t[]){0x04}, 1},
    {0xB2, (uint8_t[]){0x10}, 1},
};

static void lcd_bsp_enable_dsi_phy_power(void) {
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id    = 3,     
        .voltage_mv = 2500,  
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on (2.5V)");
}

esp_lcd_panel_handle_t app_lcd_init(void) {
    ESP_LOGI(TAG, "Initializing EK79007 MIPI-DSI Panel (1024x600)...");

    gpio_config_t gpio_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << LCD_BL_PIN) | (1ULL << LCD_PWREN_PIN),
    };
    gpio_config(&gpio_conf);

    gpio_set_level(LCD_BL_PIN, 0);      
    gpio_set_level(LCD_PWREN_PIN, 1);   
    vTaskDelay(pdMS_TO_TICKS(100));     

    lcd_bsp_enable_dsi_phy_power();

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = 0,
        .num_data_lanes     = 2,                              
        .phy_clk_src        = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT, 
        .lane_bit_rate_mbps = 700,                            
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    esp_lcd_panel_io_handle_t mipi_dbi_io;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    gpio_config_t rst_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_RST_PIN),
    };
    gpio_config(&rst_conf);

    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "Sending EK79007 Initialization Commands...");
    for (int i = 0; i < EK79007_INIT_CMD_LEN; i++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(mipi_dbi_io, 
                                                   ek79007_init_cmds[i].cmd, 
                                                   ek79007_init_cmds[i].data, 
                                                   ek79007_init_cmds[i].data_bytes));
    }

    esp_lcd_panel_handle_t mipi_dpi_panel;
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 50,                          
        .in_color_format    = LCD_COLOR_FMT_RGB888,        
        .out_color_format   = LCD_COLOR_FMT_RGB888,       
        .num_fbs            = 2,                           
        .video_timing       = {
            .h_size            = 1024,                      
            .v_size            = 600,                       
            .hsync_back_porch  = 160,                      
            .hsync_front_porch = 160,                      
            .hsync_pulse_width = 20,                       
            .vsync_back_porch  = 23,                       
            .vsync_front_porch = 12,                       
            .vsync_pulse_width = 10,                       
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &mipi_dpi_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(mipi_dpi_panel));
    /* ESP-IDF v6.0 起 extra_dpi_panel_flags.use_dma2d 被移除, 改用单独接口开 DMA2D */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(mipi_dpi_panel));

    gpio_set_level(LCD_BL_PIN, 1);     
    ESP_LOGI(TAG, "LCD Initialization Complete (1024x600 RGB888, DMA2D ON), Backlight ON");

    return mipi_dpi_panel;
}

/* ============================================================================
 * 颜色自检: 用来判断"红/蓝是否互换"以及"有没有偏色"
 *   自检条按 LCD_PIXEL_ORDER_BGR 指定的内存字节序写入(与解码端保持一致),
 *   所以正常应显示 红|绿|蓝; 若显示 蓝|绿|红, 就是开关设反了。
 *   像素写入统一用 app_pixel.h 里的 app_pixel_put()
 * ========================================================================== */
void lcd_color_selftest(esp_lcd_panel_handle_t panel)
{
#if LCD_COLOR_SELFTEST
    if (!panel) {
        return;
    }

    const int w = LCD_H_RES;
    const int h = LCD_V_RES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "颜色自检: 内存不足");
        return;
    }

    /* 上半屏: 三段竖条 红 / 绿 / 蓝 */
    for (int y = 0; y < h / 2; y++) {
        uint8_t *row = buf + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + (size_t)x * 3;
            if (x < w / 3) {
                app_pixel_put(p, 255, 0, 0);      /* 红 */
            } else if (x < (w * 2) / 3) {
                app_pixel_put(p, 0, 255, 0);      /* 绿 */
            } else {
                app_pixel_put(p, 0, 0, 255);      /* 蓝 */
            }
        }
    }

    /* 下半屏: 左白右灰, 用来看有没有整体偏色(BT601/BT709 之类) */
    for (int y = h / 2; y < h; y++) {
        uint8_t *row = buf + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            uint8_t v = (x < w / 2) ? 255 : 128;
            app_pixel_put(row + (size_t)x * 3, v, v, v);
        }
    }

    ESP_LOGI(TAG, "颜色自检: 上半屏左->右应为 红|绿|蓝, 下半屏左白右灰 (显示成 蓝|绿|红 就是 LCD_PIXEL_ORDER_BGR 设反了)");
    esp_lcd_panel_draw_bitmap(panel, 0, 0, w, h, buf);
    vTaskDelay(pdMS_TO_TICKS(2000));

    free(buf);
#endif
}