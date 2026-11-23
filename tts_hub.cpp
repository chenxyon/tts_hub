/**
 * @file     tts_hub.cpp
 * @brief    TTS 中心实现 —— 厂商无关的统一入口
 *
 * 播放侧：收到驱动回调的 PCM（16k）后线性重采样到喇叭采样率再送输出槽。
 *
 * 策略：
 *   - 固定话术 say()：有内置离线片段→直接播（离线、低延迟）；
 *     无片段→在线则走云 TTS，离线则仅日志提示。
 *   - 任意文本 speak()：走云 TTS（需联网）。
 *
 * 修改：2026-11-21 重构为 TtsHub 静态类
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "tts_hub.h"
#include "tts_config.h"
#include "tts_xfyun.h"

static const char *TAG = "TTS_HUB";

/* ==================== 静态成员初始化 ==================== */

TtsPcmSink TtsHub::s_sink = nullptr;
uint32_t TtsHub::s_outRate = 22050;      /* 输出采样率 */
volatile bool TtsHub::s_online = true;   /* 上层用 setOnline 同步 */

/* 厂商表：新增厂商时加一行 + 实现对应驱动即可 */
static const TtsDriver *providerTable()
{
    static const TtsDriver *table[] = {
        &TtsXfyun::driver(),  /* TtsProvider::XFYUN */
        nullptr,              /* TtsProvider::BAIDU 待实现 */
        nullptr,              /* TtsProvider::ALI   待实现 */
    };
    /* 静态断言：表大小与厂商枚举一致 */
    static_assert(sizeof(table) / sizeof(table[0]) == (int)TtsProvider::ALI + 1,
                  "provider table out of sync");
    return table;
}

const TtsDriver *TtsHub::activeDriver()
{
    TtsProvider p = TTS_ACTIVE_PROVIDER;
    if ((int)p < 0 || (int)p > (int)TtsProvider::ALI) {
        return nullptr;
    }
    return providerTable()[(int)p];
}

/* ==================== 初始化与状态 ==================== */

esp_err_t TtsHub::init(TtsPcmSink sink, uint32_t outputRateHz)
{
    if (sink == nullptr) return ESP_ERR_INVALID_ARG;
    s_sink = sink;
    if (outputRateHz != 0) s_outRate = outputRateHz;

    const TtsDriver *d = activeDriver();
    if (d == nullptr) {
        ESP_LOGE(TAG, "厂商未实现 (id=%d)", (int)TTS_ACTIVE_PROVIDER);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "当前厂商: %s  输出采样率=%lu", d->name, (unsigned long)s_outRate);
    return d->init ? d->init() : ESP_OK;
}

const char *TtsHub::providerName()
{
    const TtsDriver *d = activeDriver();
    return d ? d->name : "none";
}

bool TtsHub::isBusy()
{
    const TtsDriver *d = activeDriver();
    return d && d->isBusy ? d->isBusy() : false;
}

void TtsHub::setOnline(bool online) { s_online = online; }
bool TtsHub::online()               { return s_online; }

/* ==================== PCM 播放（含重采样） ==================== */

/* 播放 16k s16le 单声道 PCM：需要时重采样到 s_outRate 后分片送输出槽 */
void TtsHub::playPcm16(const int16_t *in, size_t samples)
{
    if (in == nullptr || samples == 0) return;
    if (s_sink == nullptr) {
        ESP_LOGE(TAG, "未注册 PCM 输出槽");
        return;
    }

    /* 采样率一致：直通，零开销、零失真（board 采样率 16000 时走这里） */
    if (s_outRate == SRC_HZ) {
        const size_t CHUNK = 2048;
        for (size_t o = 0; o < samples; o += CHUNK) {
            size_t n = (samples - o) > CHUNK ? CHUNK : (samples - o);
            s_sink(in + o, n * sizeof(int16_t));
        }
        return;
    }

    const uint32_t DST = s_outRate;
    size_t out_n = samples * DST / SRC_HZ;
    int16_t *out = (int16_t *)malloc(out_n * sizeof(int16_t));
    if (out == nullptr) {  /* 内存不足则按原采样直接送输出槽（会变调，仅保底） */
        s_sink(in, samples * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < out_n; i++) {
        float pos = (float)i * SRC_HZ / DST;
        size_t idx = (size_t)pos;
        float frac = pos - idx;
        int16_t a = in[idx < samples ? idx : samples - 1];
        int16_t b = in[(idx + 1) < samples ? idx + 1 : samples - 1];
        out[i] = (int16_t)(a + (b - a) * frac);
    }
    const size_t CHUNK = 2048;
    for (size_t o = 0; o < out_n; o += CHUNK) {
        size_t n = (out_n - o) > CHUNK ? CHUNK : (out_n - o);
        s_sink(out + o, n * sizeof(int16_t));
    }
    free(out);
}

/* 云 TTS 解出 PCM 的回调（src 固定 16k） */
void TtsHub::cloudPlayCb(const int16_t *in, size_t samples, uint32_t srcHz, void *ctx)
{
    (void)srcHz;
    (void)ctx;
    playPcm16(in, samples);
}

/* ==================== 长文本分段 ==================== */

/* 返回 UTF-8 字符字节数（按首字节判断，非法则当 1 处理） */
size_t TtsHub::u8Len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 若 p 处开始是一个"句读标点"，返回其字节数；否则 0。
 * 分段时在标点之后断开，朗读更自然。 */
size_t TtsHub::punctLen(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] == '\n' || u[0] == '.' || u[0] == '!' || u[0] == '?' ||
        u[0] == ';' || u[0] == ',' || u[0] == ' ') {
        return 1;
    }
    if (u[0] == 0xE3 && u[1] == 0x80 && (u[2] == 0x82 || u[2] == 0x81)) return 3; /* 。、 */
    if (u[0] == 0xEF && u[1] == 0xBC) {                                            /* ，！？；： */
        unsigned char c = u[2];
        if (c == 0x8C || c == 0x81 || c == 0x9F || c == 0x9B || c == 0x9A) return 3;
    }
    return 0;
}

/* 从 s 中取一段，长度不超过 maxb：优先在标点后断开，否则按 UTF-8 边界硬切。
 * 返回该段字节数（至少 1 个字符）。 */
size_t TtsHub::nextSegmentLen(const char *s, size_t total, size_t maxb)
{
    if (total <= maxb) return total;

    size_t cut = 0, i = 0;
    while (i < total && i < maxb) {
        size_t pl = punctLen(s + i);
        size_t cl = pl ? pl : u8Len((unsigned char)s[i]);
        if (pl) cut = i + pl;              /* 分割点 = 标点之后 */
        i += cl;
    }
    if (cut > 0) return cut;

    size_t c = 0;                          /* 无标点：UTF-8 边界硬切 */
    while (c < total) {
        size_t cl = u8Len((unsigned char)s[c]);
        if (c + cl > maxb) break;
        c += cl;
    }
    return c ? c : u8Len((unsigned char)s[0]);
}

/* ==================== 任意文本合成 ==================== */

esp_err_t TtsHub::speak(const char *text)
{
    const TtsDriver *d = activeDriver();
    if (d == nullptr || d->speak == nullptr) return ESP_ERR_NOT_SUPPORTED;
    if (s_sink == nullptr) {
        ESP_LOGE(TAG, "未注册 PCM 输出槽");
        return ESP_ERR_INVALID_STATE;
    }
    if (text == nullptr) return ESP_ERR_INVALID_ARG;
    if (!s_online) {
        ESP_LOGW(TAG, "离线，无法合成任意文本: %s", text);
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = strlen(text);
    if (total == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t last = ESP_OK;
    size_t off = 0;
    int seg = 0;
    while (off < total && s_online) {
        size_t len = nextSegmentLen(text + off, total - off, SEG_MAX_BYTES);
        if (len == 0) break;

        char segbuf[SEG_MAX_BYTES + 4];
        memcpy(segbuf, text + off, len);
        segbuf[len] = '\0';

        if (seg > 0) vTaskDelay(pdMS_TO_TICKS(200));  /* 让上一段 TLS 收尾再开下一段 */
        ESP_LOGI(TAG, "[%s] 分段合成 %d：%u 字节", d->name, seg, (unsigned)len);
        esp_err_t r = d->speak(segbuf, cloudPlayCb, nullptr);
        if (r != ESP_OK) {
            last = r;
            ESP_LOGW(TAG, "第 %d 段合成失败(%s)，继续后续段", seg, esp_err_to_name(r));
        }
        off += len;
        seg++;
    }

    ESP_LOGI(TAG, "[%s] 播放%s（共 %d 段）", d->name,
             last == ESP_OK ? "完成" : "有失败", seg);
    return last;
}

/* ==================== 固定话术 ==================== */

static bool offlineHave(TtsPhrase ph)
{
    int idx = (int)ph;
    return idx >= 0 && idx < TtsPhrases::COUNT &&
           TtsPhrases::clips[idx].data != nullptr &&
           TtsPhrases::clips[idx].len >= 2;
}

bool TtsHub::hasClip(TtsPhrase ph)
{
    return offlineHave(ph);
}

esp_err_t TtsHub::say(TtsPhrase ph)
{
    if ((int)ph >= TtsPhrases::COUNT) return ESP_ERR_INVALID_ARG;

    /* 1) 离线片段优先：离线、低延迟、断网也能播 */
    if (offlineHave(ph)) {
        const TtsClip &clip = TtsPhrases::clips[(int)ph];
        ESP_LOGI(TAG, "离线片段播放: %s", TtsPhrases::text(ph));
        playPcm16((const int16_t *)clip.data, clip.len / 2);
        return ESP_OK;
    }
    /* 2) 无片段：在线走云 TTS，离线仅日志 */
    const char *text = TtsPhrases::text(ph);
    if (s_online) {
        ESP_LOGI(TAG, "无离线片段，回退云 TTS: %s", text);
        return speak(text);
    }
    ESP_LOGW(TAG, "离线且无片段，跳过: %s", text);
    return ESP_ERR_NOT_FOUND;
}

/* ==================== 异步封装 ==================== */

struct TtsAsyncArg {
    char *text;
    TtsPhrase ph;
    bool isPhrase;
};

void TtsHub::asyncTask(void *arg)
{
    TtsAsyncArg *a = (TtsAsyncArg *)arg;
    if (a->isPhrase) {
        say(a->ph);
    } else {
        speak(a->text);
    }
    free(a->text);
    free(a);
    vTaskDelete(nullptr);
}

esp_err_t TtsHub::speakAsync(const char *text)
{
    if (text == nullptr) return ESP_ERR_INVALID_ARG;
    if (isBusy()) return ESP_ERR_INVALID_STATE;
    TtsAsyncArg *a = (TtsAsyncArg *)malloc(sizeof(TtsAsyncArg));
    if (a == nullptr) return ESP_ERR_NO_MEM;
    a->text = strdup(text);
    a->ph = TtsPhrase::OK;
    a->isPhrase = false;
    if (a->text == nullptr) {
        free(a);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(asyncTask, "tts_hub", 8192, a, 6, nullptr) != pdPASS) {
        free(a->text);
        free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t TtsHub::sayAsync(TtsPhrase ph)
{
    if (isBusy()) return ESP_ERR_INVALID_STATE;
    TtsAsyncArg *a = (TtsAsyncArg *)malloc(sizeof(TtsAsyncArg));
    if (a == nullptr) return ESP_ERR_NO_MEM;
    a->text = nullptr;
    a->ph = ph;
    a->isPhrase = true;
    if (xTaskCreate(asyncTask, "tts_hub", 8192, a, 6, nullptr) != pdPASS) {
        free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#include "tts_hub_version.h"
#include "version_manager.h"
COMPONENT_VERSION_REGISTER(tts_hub, TTS_HUB_VERSION_STRING)
