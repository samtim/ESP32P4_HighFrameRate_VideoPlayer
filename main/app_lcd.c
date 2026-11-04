#include "app_lcd.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "app_lcd";

#define LCD_BL_PIN       GPIO_NUM_23  // 背光控制脚
#define LCD_RST_PIN      GPIO_NUM_22  // 复位脚
#define LCD_PWREN_PIN    GPIO_NUM_21  // 屏幕上电脚

#define EK79007_INIT_CMD_LEN  9

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
} mipi_lcd_init_cmd_t;

// 慧勤智远官方 EK79007 初始化序列
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
        .chan_id    = 3,     // LDO3
        .voltage_mv = 2500,  // 2.5V
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on (2.5V)");
}

esp_lcd_panel_handle_t app_lcd_init(void) {
    ESP_LOGI(TAG, "Initializing EK79007 MIPI-DSI Panel (1024x600)...");

    // 1. 引脚配置
    gpio_config_t gpio_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << LCD_BL_PIN) | (1ULL << LCD_PWREN_PIN),
    };
    gpio_config(&gpio_conf);

    gpio_set_level(LCD_BL_PIN, 0);      // 背光关闭
    gpio_set_level(LCD_PWREN_PIN, 1);   // 上电
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. DSI PHY 电源
    lcd_bsp_enable_dsi_phy_power();

    // 3. DSI 总线
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = 0,
        .num_data_lanes     = 2,                              // 2 Lane
        .phy_clk_src        = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT, // XTAL 晶振
        .lane_bit_rate_mbps = 700,                            // 700 Mbps
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    // 4. DBI 控制面板
    esp_lcd_panel_io_handle_t mipi_dbi_io;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    // 5. 复位时序
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

    // 发送初始化序列
    ESP_LOGI(TAG, "Sending EK79007 Initialization Commands...");
    for (int i = 0; i < EK79007_INIT_CMD_LEN; i++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(mipi_dbi_io, 
                                                   ek79007_init_cmds[i].cmd, 
                                                   ek79007_init_cmds[i].data, 
                                                   ek79007_init_cmds[i].data_bytes));
    }

    // 6. 创建 DPI 面板 (配置为 RGB888 格式匹配解码输出，开启 DMA2D 加速)
    esp_lcd_panel_handle_t mipi_dpi_panel;
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 50,                          // 50MHz
        .in_color_format    = LCD_COLOR_FMT_RGB888,        // RGB888
        .out_color_format   = LCD_COLOR_FMT_RGB888,       // RGB888
        .num_fbs            = 2,                           // 2 帧缓存
        .video_timing       = {
            .h_size            = 1024,                      // 1024
            .v_size            = 600,                       // 600
            .hsync_back_porch  = 160,                      // hbp 160
            .hsync_front_porch = 160,                      // hfp 160
            .hsync_pulse_width = 20,                       // hsw 20
            .vsync_back_porch  = 23,                       // vbp 23
            .vsync_front_porch = 12,                       // vfp 12
            .vsync_pulse_width = 10,                       // vsw 10
        },
        .flags = {
            .use_dma2d = true,                             // 开启 DMA2D 硬件加速
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &mipi_dpi_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(mipi_dpi_panel));

    gpio_set_level(LCD_BL_PIN, 1);     // 打开背光
    ESP_LOGI(TAG, "LCD Initialization Complete (1024x600 RGB888, DMA2D ON), Backlight ON");

    return mipi_dpi_panel;
}