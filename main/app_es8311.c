/**
 ******************************************************************************
 * @file        app_es8311.c
 * @brief       ES8311 codec + I2S 输出硬件层
 *
 * 寄存器序列与官方驱动 esp_codec_dev/device/es8311/es8311.c 的 open()+start()
 * 保持一致 (厂商 24_music 例程用的就是那份驱动)。关键点:
 *   * REG00 bit7 = CSM_ON 必须为 1 (0x80), 否则芯片状态机不启动 -> 完全无声
 *   * REG0D = 0x01 模拟电路上电, 不写 -> 完全无声
 *   * REG17/REG15/REG14/REG45 等配套上电值
 *   * REG02 时钟分频: MCLK = 256 * fs 时为 0x00 (pre_div=1, pre_multi=x1)
 *   * REG01 = 0x3F (MCLK 取自 MCLK 引脚, 不反相)
 *   * REG37 是 DAC ramp rate, 官方写 0x08; 静音由 REG31 bit6/5 控制
 ******************************************************************************
 */

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "app_es8311.h"

static const char *TAG = "audio_hw";

static i2c_master_dev_handle_t s_es8311_dev = NULL;
static i2s_chan_handle_t       s_tx_handle  = NULL;

/* ============================================================================
 * ES8311 寄存器地址
 * ========================================================================== */
#define ES8311_REG00_RESET      0x00    /* [7]=CSM_ON  [6]=1:Master 0:Slave */
#define ES8311_REG01_MCLK       0x01    /* [7]=MCLK源(0:引脚)  [6]=MCLK 反相 */
#define ES8311_REG02_PRE_DIV    0x02    /* [7:5]=pre_div  [4:3]=pre_multi */
#define ES8311_REG03_ADC_OSR    0x03
#define ES8311_REG04_DAC_OSR    0x04
#define ES8311_REG05_DIV        0x05    /* [7:4]=adc_div  [3:0]=dac_div */
#define ES8311_REG06_SCLK_DIV   0x06
#define ES8311_REG07_LRCK_H     0x07
#define ES8311_REG08_LRCK_L     0x08
#define ES8311_REG09_SDP_IN     0x09    /* DAC 数据口: [1:0]格式 [3:2]位宽 [6]=1 关闭 */
#define ES8311_REG0A_SDP_OUT    0x0A    /* ADC 数据口 (本工程不用) */
#define ES8311_REG0B_SYS        0x0B
#define ES8311_REG0C_SYS        0x0C
#define ES8311_REG0D_ANA_PWR    0x0D    /* [0]=1 模拟电路上电 */
#define ES8311_REG0E_SYS        0x0E
#define ES8311_REG10_SYS        0x10
#define ES8311_REG11_SYS        0x11
#define ES8311_REG12_SYS        0x12    /* DAC 使能 */
#define ES8311_REG13_SYS        0x13
#define ES8311_REG14_SYS        0x14
#define ES8311_REG15_ADC        0x15
#define ES8311_REG16_ADC_GAIN   0x16
#define ES8311_REG17_ADC_VOL    0x17
#define ES8311_REG1B_ADC        0x1B
#define ES8311_REG1C_ADC        0x1C
#define ES8311_REG31_DAC_MUTE   0x31    /* [6:5]=静音 */
#define ES8311_REG32_DAC_VOL    0x32
#define ES8311_REG37_DAC_RAMP   0x37    /* DAC ramp rate, 官方用 0x08 */
#define ES8311_REG44_GPIO       0x44    /* dac2adc 参考 */
#define ES8311_REG45_GPIO       0x45    /* GP control */

#define ES8311_TRY(x) \
    do { esp_err_t __err = (x); if (__err != ESP_OK) { return __err; } } while (0)

/* ============================================================================
 * 寄存器读写
 * ========================================================================== */
static esp_err_t es8311_write_reg(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    esp_err_t err = i2c_master_transmit(s_es8311_dev, write_buf, sizeof(write_buf),
                                        pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 write REG[0x%02X]=0x%02X failed: %s", reg_addr, data, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t es8311_read_reg(uint8_t reg_addr, uint8_t *data)
{
    return i2c_master_transmit_receive(s_es8311_dev, &reg_addr, 1, data, 1, pdMS_TO_TICKS(100));
}

/* 回读关键寄存器: 用来确认 I2C 写入真的生效了 (而不是"写了但芯片没收到") */
static void es8311_dump_regs(void)
{
    static const uint8_t regs[] = {
        ES8311_REG00_RESET, ES8311_REG01_MCLK, ES8311_REG02_PRE_DIV, ES8311_REG05_DIV,
        ES8311_REG06_SCLK_DIV, ES8311_REG09_SDP_IN, ES8311_REG0D_ANA_PWR,
        ES8311_REG12_SYS, ES8311_REG13_SYS, ES8311_REG31_DAC_MUTE,
        ES8311_REG32_DAC_VOL, ES8311_REG37_DAC_RAMP, ES8311_REG44_GPIO,
    };
    ESP_LOGI(TAG, "---- ES8311 关键寄存器回读 (期望 00=80 01=3F 02=00 05=00 06=03 09=0C "
                  "0D=01 12=00 13=10 31=00 32=BF 37=08 44=58) ----");
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t val = 0;
        if (es8311_read_reg(regs[i], &val) == ESP_OK) {
            ESP_LOGI(TAG, "    REG[0x%02X] = 0x%02X", regs[i], val);
        } else {
            ESP_LOGE(TAG, "    REG[0x%02X] = <读失败>", regs[i]);
        }
    }
}

/* ============================================================================
 * ES8311 初始化
 * ========================================================================== */
static esp_err_t es8311_init_codec(void)
{
    uint8_t regv = 0;

    /* ---- 1. I2C 主机总线 ---- */
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = AUDIO_I2C_PORT,
        .scl_io_num = AUDIO_I2C_SCL_IO,
        .sda_io_num = AUDIO_I2C_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &bus_handle));

    /* ---- 2. 先探测器件是否在线: 不应答说明 SDA/SCL 接反、没供电、或 CE 决定了别的地址 ---- */
    esp_err_t err = i2c_master_probe(bus_handle, ES8311_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 在 0x%02X 无应答 (%s)！请检查 SDA=GPIO%d / SCL=GPIO%d 是否接反、"
                      "codec 3.3V 供电、CE 引脚电平(CE=0 -> 0x18, CE=1 -> 0x19)",
                 ES8311_I2C_ADDR, esp_err_to_name(err), AUDIO_I2C_SDA_IO, AUDIO_I2C_SCL_IO);
        return err;
    }
    ESP_LOGI(TAG, "ES8311 在 I2C 地址 0x%02X 应答正常", ES8311_I2C_ADDR);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_es8311_dev));

    /* ---- 3. 软复位 ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG00_RESET, 0x1F));
    vTaskDelay(pdMS_TO_TICKS(20));
    ES8311_TRY(es8311_write_reg(ES8311_REG00_RESET, 0x00));

    /* ---- 4. 先把模拟部分置为掉电态, 再从干净状态重新上电 (官方 open() 做法) ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG0D_ANA_PWR, 0xFA));

    /* ---- 5. I2C 抗干扰: 0x44 连写两次 (官方注释: 首次对 ES8311 的 I2C 写可能失败) ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG44_GPIO, 0x08));
    ES8311_TRY(es8311_write_reg(ES8311_REG44_GPIO, 0x08));

    /* ---- 6. 时钟 / 系统基础配置 (顺序与官方 open() 一致) ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG01_MCLK, 0x30));
    ES8311_TRY(es8311_write_reg(ES8311_REG02_PRE_DIV, 0x00));
    ES8311_TRY(es8311_write_reg(ES8311_REG03_ADC_OSR, 0x10));
    ES8311_TRY(es8311_write_reg(ES8311_REG16_ADC_GAIN, 0x24));
    ES8311_TRY(es8311_write_reg(ES8311_REG04_DAC_OSR, 0x10));
    ES8311_TRY(es8311_write_reg(ES8311_REG05_DIV, 0x00));
    ES8311_TRY(es8311_write_reg(ES8311_REG0B_SYS, 0x00));
    ES8311_TRY(es8311_write_reg(ES8311_REG0C_SYS, 0x00));
    ES8311_TRY(es8311_write_reg(ES8311_REG10_SYS, 0x1F));
    ES8311_TRY(es8311_write_reg(ES8311_REG11_SYS, 0x7F));

    /* ---- 7. 打开 CSM(bit7=1) 并设为 Slave(bit6=0): BCLK/LRCK 由 ESP32-P4 输出 ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG00_RESET, 0x80));

    /* ---- 8. MCLK 源 = MCLK 引脚, 不反相 ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG01_MCLK, 0x3F));

    /* ---- 9. 其它系统寄存器 ---- */
    ES8311_TRY(es8311_write_reg(ES8311_REG13_SYS, 0x10));
    ES8311_TRY(es8311_write_reg(ES8311_REG1B_ADC, 0x0A));
    ES8311_TRY(es8311_write_reg(ES8311_REG1C_ADC, 0x6A));
    ES8311_TRY(es8311_write_reg(ES8311_REG44_GPIO, 0x58));      /* 内部参考 ADCL+DACR */

    /* ---- 10. 采样格式: 16bit + 标准 I2S (Philips) ---- */
    ES8311_TRY(es8311_read_reg(ES8311_REG09_SDP_IN, &regv));
    ES8311_TRY(es8311_write_reg(ES8311_REG09_SDP_IN, (uint8_t)((regv & 0xF0) | 0x0C)));
    ES8311_TRY(es8311_read_reg(ES8311_REG0A_SDP_OUT, &regv));
    ES8311_TRY(es8311_write_reg(ES8311_REG0A_SDP_OUT, (uint8_t)((regv & 0xF0) | 0x0C)));

    /* ---- 11. 时钟分频: MCLK = 256 x fs ----
     *        官方系数表里 256xfs 的采样率都是 pre_div=1, pre_multi=x1, adc/dac_div=1,
     *        lrck=0xFF, bclk_div=4 -> REG02=0x00 REG05=0x00 REG07=0x00 REG08=0xFF */
    ES8311_TRY(es8311_read_reg(ES8311_REG02_PRE_DIV, &regv));
    ES8311_TRY(es8311_write_reg(ES8311_REG02_PRE_DIV, (uint8_t)(regv & 0x07)));
    ES8311_TRY(es8311_write_reg(ES8311_REG05_DIV, 0x00));
    ES8311_TRY(es8311_write_reg(ES8311_REG03_ADC_OSR, 0x10));
    ES8311_TRY(es8311_write_reg(ES8311_REG04_DAC_OSR, 0x10));
    ES8311_TRY(es8311_write_reg(ES8311_REG07_LRCK_H, 0x00));
    ES8311_TRY(es8311_write_reg(ES8311_REG08_LRCK_L, 0xFF));
    ES8311_TRY(es8311_write_reg(ES8311_REG06_SCLK_DIV, 0x03));  /* BCLK 不反相 + bclk_div=4 */

    /* ---- 12. 使能 DAC 数据输入 / 上电 / 解除静音 (对应官方 start()) ---- */
    ES8311_TRY(es8311_read_reg(ES8311_REG09_SDP_IN, &regv));
    ES8311_TRY(es8311_write_reg(ES8311_REG09_SDP_IN, (uint8_t)(regv & ~0x40)));  /* bit6=0: DAC 数据使能 */
    ES8311_TRY(es8311_read_reg(ES8311_REG0A_SDP_OUT, &regv));
    ES8311_TRY(es8311_write_reg(ES8311_REG0A_SDP_OUT, (uint8_t)(regv | 0x40)));  /* ADC 不用, 关掉 */

    ES8311_TRY(es8311_write_reg(ES8311_REG17_ADC_VOL, 0xBF));
    ES8311_TRY(es8311_write_reg(ES8311_REG0E_SYS, 0x02));
    ES8311_TRY(es8311_write_reg(ES8311_REG12_SYS, 0x00));       /* DAC 使能 */
    ES8311_TRY(es8311_write_reg(ES8311_REG14_SYS, 0x1A));       /* 模拟 PGA / DMIC 选择 */
    ES8311_TRY(es8311_write_reg(ES8311_REG0D_ANA_PWR, 0x01));   /* 模拟电路上电 */
    ES8311_TRY(es8311_write_reg(ES8311_REG15_ADC, 0x40));
    ES8311_TRY(es8311_write_reg(ES8311_REG37_DAC_RAMP, 0x08));  /* DAC ramp rate */
    ES8311_TRY(es8311_write_reg(ES8311_REG45_GPIO, 0x00));

    /* 解除静音: REG31 的 bit6/bit5 (官方 es8311_set_mute(false) 的做法) */
    ES8311_TRY(es8311_read_reg(ES8311_REG31_DAC_MUTE, &regv));
    ES8311_TRY(es8311_write_reg(ES8311_REG31_DAC_MUTE, (uint8_t)(regv & 0x9F)));

    /* DAC 数字音量 */
    ES8311_TRY(es8311_write_reg(ES8311_REG32_DAC_VOL, AUDIO_DAC_VOL_0DB));

    es8311_dump_regs();
    ESP_LOGI(TAG, "ES8311 初始化完成 (Slave / I2S-Philips / 16bit / MCLK=256xfs)");
    return ESP_OK;
}

/* ============================================================================
 * PA 功放
 * ========================================================================== */
static void pa_init(void)
{
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_PA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(AUDIO_PA_GPIO, 1);
    ESP_LOGI(TAG, "PA 功放使能 (GPIO %d = HIGH)", AUDIO_PA_GPIO);
}

/* ============================================================================
 * I2S 输出
 * ========================================================================== */
static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* 无数据时自动补 0, 避免功放放大底噪 */
    /* DMA 缓冲加深: 8 x 512 帧 = 16KB, 约 93ms @44.1k/16bit/立体声
     * 默认 6 x 240 帧只有 ~33ms, 视频解码/刷屏一抢占就欠载 -> 声音断续 */
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_DEFAULT_RATE),   /* MCLK = 256 x fs */
        /* ES8311 默认是标准 I2S (Philips) 格式, 用 MSB(左对齐) 会整体错位 1 个 bit */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_IO,
            .bclk = AUDIO_I2S_BCLK_IO,
            .ws   = AUDIO_I2S_WS_IO,
            .dout = AUDIO_I2S_DOUT_IO,   /* -> codec SDIN */
            .din  = AUDIO_I2S_DIN_IO,    /* <- codec SDOUT */
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));

    ESP_LOGI(TAG, "I2S 已启动: %d Hz / 16bit / Philips, MCLK:%d BCLK:%d WS:%d DOUT:%d DIN:%d",
             AUDIO_DEFAULT_RATE, AUDIO_I2S_MCLK_IO, AUDIO_I2S_BCLK_IO,
             AUDIO_I2S_WS_IO, AUDIO_I2S_DOUT_IO, AUDIO_I2S_DIN_IO);
    return ESP_OK;
}

/* ============================================================================
 * 对外接口
 * ========================================================================== */
esp_err_t app_audio_hw_init(void)
{
    pa_init();

    esp_err_t err = i2s_init();
    if (err != ESP_OK) {
        return err;
    }

    /* MCLK 已经跑起来了, 再去配 codec 的时钟树 */
    return es8311_init_codec();
}

i2s_chan_handle_t app_audio_hw_tx_handle(void)
{
    return s_tx_handle;
}

esp_err_t app_audio_hw_set_rate(uint32_t rate)
{
    if (rate < 8000 || rate > 96000) {
        ESP_LOGW(TAG, "采样率 %u Hz 超出支持范围(8k~96k), 保持 %d Hz",
                 (unsigned)rate, AUDIO_DEFAULT_RATE);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2s_channel_disable(s_tx_handle) != ESP_OK) {
        return ESP_FAIL;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);   /* MCLK = 256 x fs */
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    i2s_channel_enable(s_tx_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2S 采样率已切到 %u Hz (MCLK=%u Hz)", (unsigned)rate, (unsigned)(rate * 256));
    } else {
        ESP_LOGE(TAG, "I2S 重设采样率失败: %s", esp_err_to_name(err));
    }
    return err;
}

void app_audio_hw_set_dac_volume(uint8_t reg_val)
{
    if (s_es8311_dev) {
        es8311_write_reg(ES8311_REG32_DAC_VOL, reg_val);
    }
}

void app_audio_hw_pa_enable(bool enable)
{
    gpio_set_level(AUDIO_PA_GPIO, enable ? 1 : 0);
}

/* ============================================================================
 * 自检音: 1kHz 方波 "嘟" 一声
 *   能听到 -> I2S / ES8311 / 功放 整条链路正常, 无声问题只可能在 WAV 文件或路径
 *   听不到 -> 是 codec 寄存器或硬件链路问题(与 WAV 无关)
 * ========================================================================== */
void app_audio_hw_selftest_tone(void)
{
#if AUDIO_SELFTEST_TONE
    #define SELFTEST_FREQ_HZ    1000
    #define SELFTEST_MS         700

    const int frames      = AUDIO_DEFAULT_RATE * SELFTEST_MS / 1000;   /* 每声道样点数 */
    const int half_period = AUDIO_DEFAULT_RATE / (SELFTEST_FREQ_HZ * 2);
    const int fade        = AUDIO_DEFAULT_RATE / 200;                  /* 5ms 淡入淡出防爆音 */
    const size_t total    = (size_t)frames * 2 * sizeof(int16_t);

    int16_t *buf = (int16_t *)heap_caps_malloc(total, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGW(TAG, "自检音: 内存不足");
        return;
    }

    for (int i = 0; i < frames; i++) {
        int32_t s = ((i / half_period) & 1) ? 9000 : -9000;
        if (i < fade) {
            s = s * i / fade;
        } else if (i > frames - fade) {
            s = s * (frames - i) / fade;
        }
        buf[2 * i]     = (int16_t)s;
        buf[2 * i + 1] = (int16_t)s;
    }

    app_audio_hw_pa_enable(true);
    ESP_LOGI(TAG, "自检音: %d Hz / %d ms (听到说明 I2S+ES8311+功放 链路正常)",
             SELFTEST_FREQ_HZ, SELFTEST_MS);

    size_t written = 0;
    for (size_t off = 0; off < total; ) {
        size_t chunk = total - off;
        if (chunk > 2048) {
            chunk = 2048;
        }
        if (i2s_channel_write(s_tx_handle, (uint8_t *)buf + off, chunk, &written,
                              pdMS_TO_TICKS(1000)) != ESP_OK) {
            ESP_LOGW(TAG, "自检音写入失败");
            break;
        }
        off += chunk;
    }

    free(buf);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "自检音播放完毕");
#endif /* AUDIO_SELFTEST_TONE */
}
