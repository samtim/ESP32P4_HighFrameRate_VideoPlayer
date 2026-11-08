/**
 ******************************************************************************
 * @file        app_audio.c
 * @brief       WAV / MP3 播放层
 *
 * 数据流:
 *   audio_source_task (prio 4, core 1, 20KB 栈)     audio_player_task (prio 6, core 0)
 *     解析头部 / MP3 探测+解码            -->  PSRAM 环形缓冲  -->  I2S DMA
 *
 * 为什么这么分工:
 *   1) I2S DMA 只缓冲 ~93ms, 若边 fread/边解码/边写 I2S, 任何一次卡顿都会欠载 -> 断续;
 *   2) MP3 解码是纯 CPU 活, 放低优先级任务并钉到 core 1, 不与 core 0 上的视频解码抢 CPU;
 *   3) minimp3 的 mp3dec_decode_frame 会在栈上放一个 mp3dec_scratch_t(约 13KB), 所以
 *      "探测首帧"和"解码"都必须在这个大栈任务里做, 绝不能放在播放任务里(否则栈溢出);
 *   4) 环形缓冲里统一放"立体声 16bit PCM", 播放任务只做搬运, 逻辑最简单。
 ******************************************************************************
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "minimp3.h"
#include "esp_ae_sonic.h"

#include "app_audio.h"
#include "app_es8311.h"

static const char *TAG = "app_audio";

/* ============================================================================
 * 参数
 * ========================================================================== */
#define AUDIO_RINGBUF_SIZE      (256 * 1024)   /* 44.1k/16bit/立体声 约 1.5 秒 */
#define AUDIO_READ_CHUNK        (8 * 1024)     /* 每次从文件读 8KB */
#define AUDIO_RECV_CHUNK        (4 * 1024)     /* 播放任务每次从环形缓冲取 4KB */
#define AUDIO_PREFILL_BYTES     (96 * 1024)    /* 起播前先垫 96KB, 防止开头就欠载 */

#define AUDIO_PLAYER_STACK      6144
#define AUDIO_PLAYER_PRIO       6              /* 高于视频读盘(5), 保证喂 DMA 不被抢 */
/* 播放任务(含 sonic 变速运算)挪到 core 1:
 * core 0 留给视频的 JPEG 硬件解码, 150% 高速档才不会因为抢 CPU 丢帧 */
#define AUDIO_PLAYER_CORE       1

/* A/V 对齐: 送进 I2S 的音频还压在 DMA 里, 真正响出来要等这些帧播完。
 * 打开后会把媒体时钟往回拨"DMA 里的帧数 / 采样率 × 倍率", 让画面和听到的声音对齐。
 * 若实测画面偏慢/偏快, 把这里改成 0 关掉即可。 */
#define AUDIO_LATENCY_COMP_ENABLE   1

/* MP3 解码需要 ~17KB 栈: minimp3 的 mp3dec_decode_frame 在栈上放了一个
 * mp3dec_scratch_t (maindata 2815 + grbuf 4608 + syn 8448 + ... ≈ 16.3KB),
 * 所以解析/探测/解码都必须在下面这个大栈任务里做, 给到 24KB 留余量 */
#define AUDIO_SOURCE_STACK      24576
#define AUDIO_SOURCE_PRIO       4              /* 低于视频读盘, SD 让路给视频 */
#define AUDIO_SOURCE_CORE       1              /* 钉在 core 1, 不与视频解码争 CPU */

#define MP3_IN_BUF              (16 * 1024)    /* MP3 码流缓冲 / WAV 读缓冲 */
#define MP3_PROBE_BYTES         (8 * 1024)     /* 探测首个有效帧的范围 */

/* ============================================================================
 * 音频源信息
 * ========================================================================== */
typedef enum {
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_WAV,
    AUDIO_FMT_MP3,
} audio_fmt_t;

typedef struct {
    uint16_t channels;          /* 1 或 2 */
    uint16_t bits_per_sample;
    uint32_t sample_rate;
    uint32_t data_offset;       /* PCM 数据 / 第一个 MP3 帧的偏移 */
    uint32_t data_size;
} wav_info_t;

typedef struct {
    audio_fmt_t fmt;
    uint16_t    channels;       /* 源声道数 */
    uint32_t    sample_rate;
    uint32_t    data_offset;
} audio_info_t;

typedef struct {
    FILE        *f;
    audio_fmt_t  fmt;           /* 播放任务判断后填入 */
    audio_info_t info;          /* 源任务探测后填入 */
} audio_src_t;

/* ============================================================================
 * 模块状态
 * ========================================================================== */
static RingbufHandle_t  s_ringbuf        = NULL;
static char             s_media_path[APP_AUDIO_PATH_LEN];
static audio_src_t      s_src;

static volatile bool     s_stop              = false;
static volatile bool     s_eof               = false;
static volatile bool     s_info_ready        = false;
static volatile bool     s_source_error      = false;
static volatile bool     s_source_running    = false;
static volatile bool     s_player_running    = false;
static volatile uint32_t s_underruns         = 0;
static volatile bool     s_paused            = false;

/* --- 变速不变调 (sonic) + 媒体主时钟 -------------------------------------
 * 时钟算法(闭环): 以"真正送进 I2S 的音频采样数"为基准 ——
 *   媒体时间 = 锚点媒体位置 + (now - 锚点墙钟) * 倍率
 * 每送出一块 PCM 就用真实的已消耗样点数把锚点重新定一次, 所以:
 *   - sonic 的实际收发比哪怕和设定倍率有细微偏差, 也只能造成一个块(≈20ms)的
 *     瞬间误差, 不会随时间累积(老做法纯按墙钟×倍率估算, 偏差会累计成失步);
 *   - 加/减档、暂停都只是把锚点按"当前媒体位置"重设, 时钟连续、不跳变;
 *   - 锚点墙钟为 0 表示冻结(暂停中), 此时只返回锚点媒体位置。
 */
static const float      k_speed_factor[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f };
static volatile int      s_speed_step    = APP_AUDIO_SPEED_STEP_DEFAULT;
static volatile float    s_speed         = 1.0f;
static volatile int64_t  s_clock_base_us = 0;   /* 锚点处的媒体位置(µs) */
static volatile int64_t  s_clock_anchor_us = 0; /* 锚点墙钟(0 = 冻结) */
static volatile bool     s_clock_started = false;
static volatile uint64_t s_media_samples = 0;   /* 已送出的媒体样点数(= 立体声帧数) */
static uint32_t          s_clock_rate    = 0;   /* 采样率, 0 表示没有音频(用墙钟兜底) */
static esp_ae_sonic_handle_t s_sonic     = NULL;
static uint32_t          s_sonic_rate    = 0;
static SemaphoreHandle_t s_speed_mutex   = NULL;   /* 保护 sonic 与倍率切换(跨任务) */

/* 音频输出延迟估计: I2S DMA 里还压着多少帧(用来把媒体时钟对齐"听到的声音") */
static volatile int32_t  s_dma_fill_frames = 0;
static uint32_t          s_dma_cap_frames  = 0;

/* ============================================================================
 * 倍速 + 媒体时钟
 * ========================================================================== */
static float speed_factor_of_step(int step)
{
    if (step < APP_AUDIO_SPEED_STEP_MIN) step = APP_AUDIO_SPEED_STEP_MIN;
    if (step > APP_AUDIO_SPEED_STEP_MAX) step = APP_AUDIO_SPEED_STEP_MAX;
    return k_speed_factor[step - 1];
}

/* 当前媒体时间(µs): 锚点位置 + 锚点以来按倍率走过的时长 */
static int64_t clock_now_us(void)
{
    int64_t base = s_clock_base_us;
    int64_t anchor = s_clock_anchor_us;
    if (anchor != 0) {
        int64_t now = esp_timer_get_time();
        if (now > anchor) {
            base += (int64_t)((double)(now - anchor) * (double)s_speed);
        }
    }
    return base;
}

/* 音频任务每送出一块 PCM 调用一次: 用真实消耗的样点数校正媒体位置 */
static void clock_sync_to_samples(void)
{
    if (s_clock_rate == 0) {
        return;
    }
    int64_t media_us = (int64_t)((uint64_t)s_media_samples * 1000000ULL / (uint64_t)s_clock_rate);
    s_clock_base_us = media_us;
    s_clock_anchor_us = s_paused ? 0 : esp_timer_get_time();
}

/* 写 I2S, 同时用"这次写阻塞了多久"反推 DMA 里还压着多少帧:
 *   写阻塞时长 dt = (DMA 已有 + 本次写入 - 容量) / 采样率
 *   => 已有 = 容量 + dt*采样率 - 本次写入
 * 只有这一个量能真实反映"声音比数据晚多少", 用于 A/V 对齐。 */
static esp_err_t audio_dma_write(const void *buf, size_t bytes, size_t *written)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = i2s_channel_write(app_audio_hw_tx_handle(), buf, bytes, written,
                                      pdMS_TO_TICKS(1000));
    int64_t dt = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        return err;
    }

    if (s_dma_cap_frames && s_clock_rate && written && *written && dt >= 2000) {
        /* 只有"写被阻塞过"才说明 DMA 满了, 这时才能反推; 没阻塞(刚起播)就不要采信 */
        int64_t w_frames = (int64_t)(*written / 4);      /* 立体声 16bit: 1 帧 = 4 字节 */
        int64_t fill = (int64_t)s_dma_cap_frames
                     + dt * (int64_t)s_clock_rate / 1000000
                     - w_frames;
        if (fill < 0) {
            fill = 0;
        } else if (fill > (int64_t)s_dma_cap_frames) {
            fill = (int64_t)s_dma_cap_frames;
        }
        /* 一阶低通: DMA 是按描述符(512 帧)释放的, 单次测量有抖动 */
        s_dma_fill_frames = (int32_t)((3 * (int64_t)s_dma_fill_frames + fill) / 4);
    }
    return ESP_OK;
}

void app_audio_set_speed_step(int step)
{
    if (step < APP_AUDIO_SPEED_STEP_MIN) {
        step = APP_AUDIO_SPEED_STEP_MIN;
    } else if (step > APP_AUDIO_SPEED_STEP_MAX) {
        step = APP_AUDIO_SPEED_STEP_MAX;
    }

    float factor = speed_factor_of_step(step);

    /* 和播放任务里的 sonic 处理互斥: 保证倍率切换只发生在两次处理之间,
     * 不会出现"时钟已经按新倍率走、音频还是旧倍率"的瞬间 */
    if (s_speed_mutex) {
        xSemaphoreTake(s_speed_mutex, portMAX_DELAY);
    }

    /* 换倍率前先把当前媒体位置固定下来(用旧倍率算到此刻), 之后按新倍率继续插值 */
    int64_t cur = clock_now_us();
    s_clock_base_us = cur;
    s_clock_anchor_us = s_paused ? 0 : esp_timer_get_time();

    if (s_sonic) {
        esp_ae_err_t err = esp_ae_sonic_set_speed(s_sonic, factor);
        if (err != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "sonic 设置倍率失败(%d), 回退到 100%%", (int)err);
            factor = 1.0f;
            esp_ae_sonic_set_speed(s_sonic, factor);
        }
    }

    s_speed_step = step;
    s_speed = factor;

    if (s_speed_mutex) {
        xSemaphoreGive(s_speed_mutex);
    }

    ESP_LOGI(TAG, "播放速度: 第 %d 档 (%d%%)", s_speed_step, (int)(factor * 100.0f + 0.5f));
}

int app_audio_get_speed_step(void)
{
    return s_speed_step;
}

float app_audio_get_speed_factor(void)
{
    return s_speed;
}

/* ============================================================================
 * 小工具
 * ========================================================================== */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 把 PCM 数据推进环形缓冲 (s_stop 时立即放弃) */
static void ring_push(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len && !s_stop) {
        if (xRingbufferSend(s_ringbuf, data + off, len - off, pdMS_TO_TICKS(100)) == pdTRUE) {
            off = len;
        }
    }
}

/* ============================================================================
 * WAV 解析
 *   注意: 不能用"固定跳过 44 字节"的写法 — WAV 里经常还有 LIST/fact 等 chunk,
 *         固定偏移会让 PCM 起点错位(只有噪音或干脆听不出内容)
 * ========================================================================== */
static bool wav_read_info(FILE *f, wav_info_t *info)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    bool got_fmt = false;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) return false;

        uint32_t size = le32(ch + 4);
        long pad = (long)(size & 1);   /* chunk 按偶数字节对齐 */

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16] = {0};
            uint32_t n = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, n, f) != n) return false;
            if (le16(fmt) != 1) {   /* 1 = PCM */
                ESP_LOGE(TAG, "WAV 不是 PCM 格式 (audio_format=%u)", le16(fmt));
                return false;
            }
            info->channels        = le16(fmt + 2);
            info->sample_rate     = le32(fmt + 4);
            info->bits_per_sample = le16(fmt + 14);
            got_fmt = true;
            fseek(f, (long)size - (long)n + pad, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            info->data_offset = (uint32_t)ftell(f);
            info->data_size   = size;
            return got_fmt;
        } else {
            fseek(f, (long)size + pad, SEEK_CUR);
        }
    }
    return false;
}

/* ============================================================================
 * 格式探测 + MP3 首帧探测(拿到采样率/声道数)
 * ========================================================================== */
static audio_fmt_t detect_format(FILE *f)
{
    uint8_t hdr[12];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fseek(f, 0, SEEK_SET);
        return AUDIO_FMT_UNKNOWN;
    }
    fseek(f, 0, SEEK_SET);

    if (memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
        return AUDIO_FMT_WAV;
    }
    if (memcmp(hdr, "ID3", 3) == 0) {
        return AUDIO_FMT_MP3;
    }
    if (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0) {   /* MPEG 帧同步字 */
        return AUDIO_FMT_MP3;
    }
    return AUDIO_FMT_UNKNOWN;
}

/* 跳过 ID3v2 标签, 返回音频数据起始偏移 */
static long mp3_skip_id3(FILE *f)
{
    uint8_t hdr[10];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return 0;
    }
    if (memcmp(hdr, "ID3", 3) != 0) {
        return 0;
    }
    /* ID3v2 大小是 4 个 7bit 字节(syncsafe) */
    long size = ((long)(hdr[6] & 0x7F) << 21) | ((long)(hdr[7] & 0x7F) << 14) |
                ((long)(hdr[8] & 0x7F) << 7) | (long)(hdr[9] & 0x7F);
    long total = 10 + size + ((hdr[5] & 0x10) ? 10 : 0);   /* 有 footer 再加 10 */
    return total;
}

/* 在前若干 KB 里找到第一个能解出来的 MP3 帧, 顺便拿到采样率/声道数
 * 注意: 内部会调用 mp3dec_decode_frame, 必须在 20KB 栈的源任务里运行 */
static bool mp3_probe(FILE *f, audio_info_t *info)
{
    bool ok = false;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(MP3_PROBE_BYTES, MALLOC_CAP_SPIRAM);
    int16_t *pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                               MALLOC_CAP_DEFAULT);
    mp3dec_t *dec = (mp3dec_t *)heap_caps_calloc(1, sizeof(mp3dec_t), MALLOC_CAP_DEFAULT);
    if (!buf || !pcm || !dec) {
        ESP_LOGE(TAG, "MP3 探测: 内存不足");
        goto out;
    }

    long base = mp3_skip_id3(f);
    fseek(f, base, SEEK_SET);
    size_t n = fread(buf, 1, MP3_PROBE_BYTES, f);

    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        mp3dec_frame_info_t fi;
        mp3dec_init(dec);
        int samples = mp3dec_decode_frame(dec, buf + i, (int)(n - i), pcm, &fi);
        if (samples > 0 && fi.hz > 0 && (fi.channels == 1 || fi.channels == 2)) {
            info->sample_rate = (uint32_t)fi.hz;
            info->channels    = (uint16_t)fi.channels;
            info->data_offset = (uint32_t)(base + (long)i);
            ESP_LOGI(TAG, "MP3 首帧: %d Hz / %d ch / %d kbps (标签结束于 %ld)",
                     fi.hz, fi.channels, fi.bitrate_kbps, base);
            ok = true;
            break;
        }
    }

out:
    free(buf);
    free(pcm);
    free(dec);
    return ok;
}

/* ============================================================================
 * 音频源任务: 读文件 -> (解码) -> 立体声 16bit PCM -> 环形缓冲
 * ========================================================================== */
/* 单声道 -> 左右复制; 双声道原样 */
static size_t to_stereo(const uint8_t *src, size_t bytes, uint16_t channels, uint8_t *dst)
{
    if (channels == 1) {
        size_t samples = bytes / 2;
        for (size_t i = 0; i < samples; i++) {
            dst[4 * i + 0] = src[2 * i + 0];
            dst[4 * i + 1] = src[2 * i + 1];
            dst[4 * i + 2] = src[2 * i + 0];
            dst[4 * i + 3] = src[2 * i + 1];
        }
        return samples * 4;
    }
    memcpy(dst, src, bytes);
    return bytes;
}

static void pump_wav(audio_src_t *src, uint8_t *pcm_out, uint8_t *read_buf)
{
    const size_t chunk = (src->info.channels == 1) ? AUDIO_READ_CHUNK / 2 : AUDIO_READ_CHUNK;

    while (!s_stop) {
        size_t n = fread(read_buf, 1, chunk, src->f);
        n &= ~(size_t)1;                     /* 16bit 对齐 */
        if (n == 0) {
            break;
        }
        size_t out = to_stereo(read_buf, n, src->info.channels, pcm_out);
        ring_push(pcm_out, out);
    }
}

static void pump_mp3(audio_src_t *src, uint8_t *pcm_out, uint8_t *read_buf)
{
    mp3dec_t *dec = (mp3dec_t *)heap_caps_calloc(1, sizeof(mp3dec_t), MALLOC_CAP_DEFAULT);
    int16_t *pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                               MALLOC_CAP_DEFAULT);
    if (!dec || !pcm) {
        ESP_LOGE(TAG, "MP3 解码: 内存不足");
        free(dec);
        free(pcm);
        return;
    }

    mp3dec_init(dec);
    size_t have = 0;
    bool eof = false;

    while (!s_stop) {
        if (!eof && have < MP3_IN_BUF) {
            size_t n = fread(read_buf + have, 1, MP3_IN_BUF - have, src->f);
            if (n == 0) {
                eof = true;
            } else {
                have += n;
            }
        }

        mp3dec_frame_info_t fi;
        int samples = mp3dec_decode_frame(dec, read_buf, (int)have, pcm, &fi);

        if (samples > 0) {
            /* samples 是"每声道"样点数; 立体声时 pcm 里是交织数据 */
            size_t bytes = (size_t)samples * (size_t)fi.channels * 2;
            size_t out = to_stereo((const uint8_t *)pcm, bytes, (uint16_t)fi.channels, pcm_out);
            ring_push(pcm_out, out);

            if (fi.frame_bytes <= 0) {
                break;                                               /* 保险: 防止死循环 */
            }
            memmove(read_buf, read_buf + fi.frame_bytes, have - fi.frame_bytes);
            have -= fi.frame_bytes;
        } else if (fi.frame_bytes > 0) {
            /* 坏帧/被跳过的数据 */
            memmove(read_buf, read_buf + fi.frame_bytes, have - fi.frame_bytes);
            have -= fi.frame_bytes;
        } else {
            /* 数据不够一个完整帧 */
            if (eof) {
                break;
            }
            if (have == MP3_IN_BUF) {
                /* 缓冲满却解不出, 说明是垃圾数据: 丢掉一半继续找同步字 */
                ESP_LOGW(TAG, "MP3: 丢弃 %d 字节无法解析的数据", MP3_IN_BUF / 2);
                memmove(read_buf, read_buf + MP3_IN_BUF / 2, MP3_IN_BUF / 2);
                have = MP3_IN_BUF / 2;
            }
        }
    }

    free(dec);
    free(pcm);
}

static void audio_source_task(void *pvParameters)
{
    audio_src_t *src = (audio_src_t *)pvParameters;
    bool ok = false;

    /* read_buf: 文件原始数据 (MP3 码流缓冲也用这块, 取 MP3_IN_BUF)
     * pcm_out : 立体声 16bit 输出 (单声道时要 x2 空间) */
    uint8_t *read_buf = (uint8_t *)heap_caps_malloc(MP3_IN_BUF, MALLOC_CAP_SPIRAM);
    uint8_t *pcm_out  = (uint8_t *)heap_caps_malloc(MP3_IN_BUF * 2, MALLOC_CAP_SPIRAM);

    if (read_buf && pcm_out) {
        /* 解析/探测都放在这个大栈任务里做(见文件头说明) */
        if (src->fmt == AUDIO_FMT_WAV) {
            wav_info_t wav = {0};
            if (wav_read_info(src->f, &wav)) {
                ESP_LOGI(TAG, "WAV: %" PRIu32 " Hz / %u ch / %u bit / %" PRIu32 " bytes",
                         wav.sample_rate, wav.channels, wav.bits_per_sample, wav.data_size);
                if (wav.bits_per_sample == 16 && (wav.channels == 1 || wav.channels == 2)) {
                    src->info.channels    = wav.channels;
                    src->info.sample_rate = wav.sample_rate;
                    src->info.data_offset = wav.data_offset;
                    ok = true;
                } else {
                    ESP_LOGE(TAG, "不支持的 WAV: 需要 16bit PCM / 单或双声道 (当前 %u ch / %u bit)",
                             wav.channels, wav.bits_per_sample);
                }
            }
        } else if (src->fmt == AUDIO_FMT_MP3) {
            ok = mp3_probe(src->f, &src->info);
            if (ok) {
                ESP_LOGI(TAG, "MP3: %" PRIu32 " Hz / %u ch",
                         src->info.sample_rate, src->info.channels);
            }
        }
    } else {
        ESP_LOGE(TAG, "音频源: 内存不足");
    }

    src->info.fmt = src->fmt;
    __sync_synchronize();
    s_info_ready = true;                       /* 通知播放任务: 采样率/声道已就绪 */

    if (ok) {
        ESP_LOGI(TAG, "源任务栈余量: %u 字节", (unsigned)uxTaskGetStackHighWaterMark(NULL));

        fseek(src->f, (long)src->info.data_offset, SEEK_SET);
        if (src->fmt == AUDIO_FMT_MP3) {
            pump_mp3(src, pcm_out, read_buf);
        } else {
            pump_wav(src, pcm_out, read_buf);
        }
        ESP_LOGI(TAG, "源任务栈余量(收尾): %u 字节", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    } else {
        s_source_error = true;
    }

    free(read_buf);
    free(pcm_out);
    s_eof = true;                              /* 数据已全部进缓冲 / 出错结束 */
    s_source_running = false;
    ESP_LOGI(TAG, "音频源结束 (数据已全部进入缓冲)");
    vTaskDelete(NULL);
}

/* ============================================================================
 * 同名音频查找: 视频 xxx.mjpeg -> xxx.wav (优先) / xxx.mp3
 * ========================================================================== */
void app_audio_make_base(const char *media_path, char *base, size_t base_size)
{
    if (!base || base_size == 0) {
        return;
    }
    snprintf(base, base_size, "%s", media_path ? media_path : "");
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash)) {      /* 只去掉最后一段扩展名, 别把目录里的点当扩展名 */
        *dot = '\0';
    }
}

bool app_audio_find_pair(const char *media_path, char *out_path, size_t out_size)
{
    static const char *const exts[] = {".wav", ".mp3"};
    char base[APP_AUDIO_PATH_LEN];

    if (!media_path || !media_path[0]) {
        return false;
    }
    app_audio_make_base(media_path, base, sizeof(base));

    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char path[APP_AUDIO_PATH_LEN + 8];
        snprintf(path, sizeof(path), "%s%s", base, exts[i]);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            if (out_path && out_size) {
                snprintf(out_path, out_size, "%s", path);
            }
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * 播放任务: 环形缓冲 -> I2S (只做搬运, 不做解码)
 * ========================================================================== */
static void audio_player_task(void *pvParameters)
{
    (void)pvParameters;

    char base[sizeof(s_media_path)];
    app_audio_make_base(s_media_path, base, sizeof(base));

    /* 声明放在所有 goto 之前, 避免 exit 路径上引用未初始化的指针 */
    FILE *f_audio = NULL;

    /* 同名配对: xxx.wav 优先, 其次 xxx.mp3; 两个都没有就不出声 */
    char path[APP_AUDIO_PATH_LEN + 8];
    if (!app_audio_find_pair(s_media_path, path, sizeof(path))) {
        ESP_LOGW(TAG, "找不到与 %s 同名的音频(%s.wav / %s.mp3)", s_media_path, base, base);
        goto exit;
    }
    f_audio = fopen(path, "rb");
    if (!f_audio) {
        ESP_LOGW(TAG, "音频文件打不开: %s", path);
        goto exit;
    }

    /* 以文件内容判断格式, 不盲信扩展名 */
    audio_fmt_t fmt = detect_format(f_audio);
    if (fmt == AUDIO_FMT_UNKNOWN) {
        ESP_LOGE(TAG, "无法识别的音频文件: %s (只支持 WAV-PCM / MP3)", path);
        goto exit;
    }

    if (!s_ringbuf) {
        s_ringbuf = xRingbufferCreateWithCaps(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_SPIRAM);
    }
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "创建音频环形缓冲失败");
        goto exit;
    }

    /* 丢弃上一轮可能残留的数据 */
    void *stale = NULL;
    size_t stale_size = 0;
    while ((stale = xRingbufferReceiveUpTo(s_ringbuf, &stale_size, 0, AUDIO_RECV_CHUNK)) != NULL) {
        vRingbufferReturnItem(s_ringbuf, stale);
    }

    s_src.f   = f_audio;
    s_src.fmt = fmt;
    memset(&s_src.info, 0, sizeof(s_src.info));

    s_eof = false;
    s_underruns = 0;
    s_info_ready = false;
    s_source_error = false;
    s_source_running = true;
    xTaskCreatePinnedToCore(audio_source_task, "audio_src", AUDIO_SOURCE_STACK, &s_src,
                            AUDIO_SOURCE_PRIO, NULL, AUDIO_SOURCE_CORE);

    /* 等源任务把采样率/声道探测出来 (WAV 要读头部, MP3 要解首帧) */
    int64_t t_wait = esp_timer_get_time();
    while (!s_info_ready && !s_stop) {
        if (esp_timer_get_time() - t_wait > 3000000) {
            ESP_LOGW(TAG, "等待音频源超时");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (!s_info_ready || s_source_error) {
        ESP_LOGE(TAG, "音频源初始化失败: %s", path);
        goto exit;
    }

    /* 采样率要等探测结果出来后再设 (I2S 时钟 + ES8311 在 slave 下无需改寄存器) */
    app_audio_hw_set_rate(s_src.info.sample_rate);

    /* 变速不变调: 建 sonic (采样率必须是 4000 或 11025 的整数倍) */
    uint32_t rate = s_src.info.sample_rate;
    if (s_sonic && s_sonic_rate != rate) {
        esp_ae_sonic_close(s_sonic);
        s_sonic = NULL;
    }
    if (s_sonic == NULL) {
        esp_ae_sonic_cfg_t sonic_cfg = {
            .sample_rate     = rate,
            .channel         = 2,       /* 环形缓冲里统一是立体声 */
            .bits_per_sample = 16,
        };
        esp_ae_err_t ae = esp_ae_sonic_open(&sonic_cfg, &s_sonic);
        if (ae != ESP_AE_ERR_OK) {
            s_sonic = NULL;
            ESP_LOGE(TAG, "sonic 初始化失败(%d): %" PRIu32 " Hz 不支持变速, 锁在 100%%", (int)ae, rate);
            /* 不能只让视频变速而音频不变速, 否则音视频就失步了 -> 强制回到 100% */
            app_audio_set_speed_step(APP_AUDIO_SPEED_STEP_DEFAULT);
        } else {
            s_sonic_rate = rate;
            esp_ae_sonic_reset(s_sonic);
            esp_ae_sonic_set_speed(s_sonic, s_speed);
        }
    } else {
        esp_ae_sonic_reset(s_sonic);
        esp_ae_sonic_set_speed(s_sonic, s_speed);
    }

    /* 预填充: 先把缓冲垫起来再起播, 这样开头不会有空洞
     * MP3 需要先解码一会儿, 所以时间上限放宽一些 */
    int64_t t_fill = esp_timer_get_time();
    while (!s_stop && !s_eof) {
        size_t buffered = AUDIO_RINGBUF_SIZE - xRingbufferGetCurFreeSize(s_ringbuf);
        if (buffered >= AUDIO_PREFILL_BYTES) {
            break;
        }
        if (esp_timer_get_time() - t_fill > 2000000) {
            ESP_LOGW(TAG, "预填充超时, 已垫 %u 字节", (unsigned)buffered);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    app_audio_hw_pa_enable(true);
    s_media_samples   = 0;
    s_clock_rate      = s_src.info.sample_rate;
    s_clock_base_us   = 0;
    s_clock_anchor_us = esp_timer_get_time();
    s_clock_started   = true;

    /* 查一下 I2S DMA 有多深: 用来估计"声音比送进去的数据晚多少" */
    i2s_chan_info_t chan_info;
    s_dma_cap_frames = 0;
    if (i2s_channel_get_info(app_audio_hw_tx_handle(), &chan_info) == ESP_OK &&
        chan_info.total_dma_buf_size > 0) {
        s_dma_cap_frames = chan_info.total_dma_buf_size / 4;   /* 立体声 16bit: 1 帧 = 4 字节 */
    }
    s_dma_fill_frames = 0;

    ESP_LOGI(TAG, "开始播放音频: %s (%" PRIu32 " Hz / %u ch, %d%%), I2S DMA %u 帧(约 %u ms)",
             path, s_src.info.sample_rate, s_src.info.channels, (int)(s_speed * 100.0f + 0.5f),
             (unsigned)s_dma_cap_frames,
             (unsigned)(s_dma_cap_frames ? s_dma_cap_frames * 1000 / s_src.info.sample_rate : 0));

    {
        static uint8_t pcm_in[AUDIO_RECV_CHUNK];        /* 内部 RAM: 环形缓冲取出的 PCM */
        static uint8_t pcm_out[AUDIO_RECV_CHUNK];       /* 内部 RAM: sonic 输出 */
        size_t bytes_written = 0;
        size_t total_bytes = 0;

        /* sonic 每次要多少个输出样点(立体声 16bit: 1 样点 = 4 字节) */
        const uint32_t out_need = AUDIO_RECV_CHUNK / 4;

        while (!s_stop) {
            if (s_paused) {
                vTaskDelay(pdMS_TO_TICKS(20));   /* 暂停: 不写 I2S, 缓冲会自然填满后阻塞源任务 */
                continue;
            }

            size_t item_size = 0;
            uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_ringbuf, &item_size,
                                                              pdMS_TO_TICKS(20), AUDIO_RECV_CHUNK);
            if (!data) {
                if (s_eof) {
                    break;          /* 源已结束 + 缓冲已抽空 -> 放完了 */
                }
                s_underruns++;      /* 缓冲被抽空: 解码/读盘供不上了 */
                continue;
            }

            item_size &= ~(size_t)1;                    /* 16bit 对齐 */
            if (item_size > 0) {
                memcpy(pcm_in, data, item_size);
            }
            vRingbufferReturnItem(s_ringbuf, data);

            if (item_size == 0) {
                continue;
            }

            if (s_sonic == NULL) {
                /* 没有 sonic: 直通(理论上不会走到, 保底能出声) */
                if (audio_dma_write(pcm_in, item_size, &bytes_written) != ESP_OK) {
                    ESP_LOGW(TAG, "i2s_channel_write 失败");
                    break;
                }
                total_bytes += bytes_written;
                s_media_samples += (uint64_t)(item_size / 4);
                clock_sync_to_samples();
                continue;
            }

            /* 变速不变调: 输入一块 PCM, 反复取输出直到输入被吃光、内部缓存也空 */
            uint32_t remain = (uint32_t)(item_size / 4);
            uint8_t *in_ptr = pcm_in;
            uint32_t cached = 1;            /* 先假设还有缓存, 至少进一次循环 */
            uint32_t consumed = 0;          /* 这块数据实际被 sonic 吃掉的样点数 */
            while ((remain > 0 || cached > 0) && !s_stop) {
                esp_ae_sonic_in_data_t in_data = {
                    .samples     = in_ptr,
                    .num         = remain,
                    .consume_num = 0,
                };
                esp_ae_sonic_out_data_t out_data = {
                    .samples    = pcm_out,
                    .needed_num = out_need,
                    .out_num    = 0,
                };
                esp_ae_err_t ae;
                if (s_speed_mutex) {
                    xSemaphoreTake(s_speed_mutex, portMAX_DELAY);
                }
                ae = esp_ae_sonic_process(s_sonic, &in_data, &out_data);
                if (s_speed_mutex) {
                    xSemaphoreGive(s_speed_mutex);
                }
                if (ae != ESP_AE_ERR_OK) {
                    ESP_LOGW(TAG, "sonic 处理失败(%d)", (int)ae);
                    goto player_done;
                }

                if (in_data.consume_num > 0) {
                    in_ptr += (size_t)in_data.consume_num * 4;
                    remain -= in_data.consume_num;
                    consumed += in_data.consume_num;
                }

                if (out_data.out_num > 0) {
                    size_t bytes = (size_t)out_data.out_num * 4;
                    if (audio_dma_write(pcm_out, bytes, &bytes_written) != ESP_OK) {
                        ESP_LOGW(TAG, "i2s_channel_write 失败");
                        goto player_done;
                    }
                    total_bytes += bytes_written;
                }

                cached = out_data.out_num;
                if (in_data.consume_num == 0 && out_data.out_num == 0) {
                    break;      /* 吃不动也吐不出, 防止死循环 */
                }
            }

            /* 用真实消耗的样点数把媒体时钟锚点校正一次(闭环, 不会积累漂移) */
            s_media_samples += consumed;
            clock_sync_to_samples();
        }

player_done:
        ESP_LOGI(TAG, "播放结束: 写入 %u 字节 (~%u 秒), 缓冲欠载 %u 次",
                 (unsigned)total_bytes,
                 (unsigned)(total_bytes / (s_src.info.sample_rate * 4)),
                 (unsigned)s_underruns);
    }

exit:
    /* 先让源任务收手再关文件, 否则它可能正在 fread 这个 FILE */
    s_stop = true;
    for (int i = 0; i < 100 && s_source_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (f_audio) {
        fclose(f_audio);
    }
    if (!s_clock_started) {
        /* 音频不可用时也要给个时钟, 让视频改用系统定时器继续播(仍按档位变速) */
        s_clock_rate      = 0;
        s_clock_base_us   = 0;
        s_clock_anchor_us = esp_timer_get_time();
        s_clock_started   = true;
    }
    s_player_running = false;
    vTaskDelete(NULL);
}

/* ============================================================================
 * 对外接口
 * ========================================================================== */
esp_err_t app_audio_play(const char *media_path)
{
    if (media_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 上一轮如果还没退干净, 先停掉并等它退出 (最多等 1s) */
    app_audio_stop();
    for (int i = 0; i < 200 && (s_player_running || s_source_running); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (s_speed_mutex == NULL) {
        s_speed_mutex = xSemaphoreCreateMutex();
        if (s_speed_mutex == NULL) {
            ESP_LOGE(TAG, "创建倍速互斥量失败, 不能变速");
        }
    }

    strlcpy(s_media_path, media_path, sizeof(s_media_path));

    s_stop           = false;
    s_eof            = false;
    s_info_ready     = false;
    s_source_error   = false;
    s_underruns      = 0;
    s_clock_base_us   = 0;
    s_clock_anchor_us = 0;
    s_clock_started   = false;
    s_media_samples   = 0;
    s_clock_rate      = 0;
    s_paused         = false;
    s_player_running = true;

    if (xTaskCreatePinnedToCore(audio_player_task, "audio_player", AUDIO_PLAYER_STACK, NULL,
                                AUDIO_PLAYER_PRIO, NULL, AUDIO_PLAYER_CORE) != pdPASS) {
        s_player_running = false;
        ESP_LOGE(TAG, "创建播放任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_audio_stop(void)
{
    s_stop = true;
}

void app_audio_set_paused(bool paused)
{
    if (paused == s_paused) {
        return;
    }
    if (paused) {
        /* 把当前媒体位置固定下来, 然后冻结(锚点墙钟清零), 暂停时间不计入 */
        s_clock_base_us = clock_now_us();
        s_clock_anchor_us = 0;
        s_paused = true;
        ESP_LOGI(TAG, "暂停");
    } else {
        /* 从冻结的媒体位置接着插值, 暂停期间不计入 */
        s_clock_anchor_us = esp_timer_get_time();
        s_paused = false;
        ESP_LOGI(TAG, "继续");
    }
}

bool app_audio_is_paused(void)
{
    return s_paused;
}

int64_t app_audio_get_clock_us(void)
{
    if (!s_clock_started) {
        return 0;
    }

    int64_t media = clock_now_us();

#if AUDIO_LATENCY_COMP_ENABLE
    /* 送进 I2S 的音频还压在 DMA 里, 真正响出来要晚 "DMA 里的帧数 / 采样率";
     * 换算成媒体时间还要乘倍率。把时钟往回拨同样多, 画面就和"听到的声音"对齐了。 */
    if (s_clock_rate) {
        int64_t lag_us = (int64_t)s_dma_fill_frames * 1000000LL / (int64_t)s_clock_rate;
        lag_us = (int64_t)((double)lag_us * (double)s_speed);
        media -= lag_us;
        if (media < 0) {
            media = 0;
        }
    }
#endif

    return media;
}

int32_t app_audio_get_output_lag_us(void)
{
#if AUDIO_LATENCY_COMP_ENABLE
    if (!s_clock_rate) {
        return 0;
    }
    int64_t lag_us = (int64_t)s_dma_fill_frames * 1000000LL / (int64_t)s_clock_rate;
    return (int32_t)((double)lag_us * (double)s_speed);
#else
    return 0;
#endif
}

bool app_audio_is_playing(void)
{
    return s_player_running;
}
