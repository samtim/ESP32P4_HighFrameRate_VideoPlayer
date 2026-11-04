/**
 ******************************************************************************
 * @file        app_es8311.h
 * @brief       ES8311 codec + I2S 输出硬件层
 *
 * 板子: 慧勤智远 ESP32-P4 小系统板 (引脚与厂商 BSP 的 myi2s.h / myes8311.h 一致)
 *
 * 使用:
 *     ESP_ERROR_CHECK(app_audio_hw_init());   // PA + I2S + ES8311
 *     app_audio_hw_selftest_tone();           // 可选: 开机"嘟"一声自检
 ******************************************************************************
 */
#ifndef __APP_ES8311_H_
#define __APP_ES8311_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2s_std.h"

/* ============================================================================
 * 硬件引脚定义 (与厂商 BSP 完全一致, 不要随意改)
 * ========================================================================== */
#define ES8311_I2C_ADDR             0x18    /* 7 位地址: CE=0 -> 0x18, CE=1 -> 0x19 */
#define AUDIO_I2C_PORT              I2C_NUM_0
#define AUDIO_I2C_SDA_IO            28
#define AUDIO_I2C_SCL_IO            29

/* ★ 注意方向: P4 的 DOUT 接 codec 的 SDIN(GPIO3); P4 的 DIN 接 codec 的 SDOUT(GPIO5)
 *   接反的话 codec 收不到任何数据, 现象就是完全无声 */
#define AUDIO_I2S_MCLK_IO           2       /* -> ES8311_MCLK */
#define AUDIO_I2S_BCLK_IO           4       /* -> ES8311_SCLK (BCLK) */
#define AUDIO_I2S_WS_IO             6       /* -> ES8311_LRCK (WS)  */
#define AUDIO_I2S_DOUT_IO           3       /* -> ES8311_SDIN  (数据进 codec) */
#define AUDIO_I2S_DIN_IO            5       /* <- ES8311_SDOUT (数据出 codec) */

#define AUDIO_PA_GPIO               11      /* 功放使能, 高有效 */

/* 默认采样率; 播放时会按 WAV 实际采样率自动切换 */
#define AUDIO_DEFAULT_RATE          44100

/* 开机 1kHz 自检音: 1 = 开启, 0 = 关闭 */
#define AUDIO_SELFTEST_TONE         0

/* DAC 数字音量寄存器值 (0x32): 0x00 = -95.5dB, 0xBF = 0dB, 0xFF = +32dB */
#define AUDIO_DAC_VOL_0DB           0xBF
#define AUDIO_DAC_VOL_MINUS_16DB    0x9F

/* ============================================================================
 * 接口
 * ========================================================================== */

/**
 * @brief  初始化音频输出硬件: PA 功放 GPIO -> I2S (先起 MCLK) -> ES8311 (Slave)
 * @note   先起 I2S 是为了让 MCLK 先跑起来, codec 的时钟树才能锁定
 */
esp_err_t app_audio_hw_init(void);

/** @brief 取 I2S 发送通道句柄 (播放层用) */
i2s_chan_handle_t app_audio_hw_tx_handle(void);

/**
 * @brief  切换 I2S 采样率
 * @note   ES8311 工作在 Slave 模式且 MCLK 恒为 256*fs, 切换采样率时 codec 寄存器不用改
 */
esp_err_t app_audio_hw_set_rate(uint32_t rate);

/** @brief 设置 DAC 数字音量, 见 AUDIO_DAC_VOL_xxx */
void app_audio_hw_set_dac_volume(uint8_t reg_val);

/** @brief PA 功放使能 (高有效) */
void app_audio_hw_pa_enable(bool enable);

/** @brief 播放 1kHz 自检音 (AUDIO_SELFTEST_TONE=0 时为空函数) */
void app_audio_hw_selftest_tone(void);

#endif /* __APP_ES8311_H_ */
