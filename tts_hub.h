/**
 * @file     tts_hub.h
 * @brief    TTS 中心 —— 厂商无关的统一语音合成入口 — C++ class
 *
 * 功能：
 *   - 任意文本合成（走云 TTS，需联网），长文本自动按标点分段
 *   - 固定话术（离线片段优先，缺失自动回退云 TTS）
 *   - 收到驱动回调的 16k PCM 后线性重采样到喇叭采样率再送输出槽
 *   - 厂商驱动表（当前：科大讯飞；百度/阿里预留）
 *
 * 修改：2026-11-21 由 C API（tts_hub_* 函数）重构为 TtsHub 静态类；
 *       话术表独立为 TtsPhrases 静态类；讯飞驱动独立为 TtsXfyun 类
 */
#ifndef TTS_HUB_H
#define TTS_HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tts_phrases.h"

/**
 * TTS 厂商枚举 —— 新增厂商时追加即可
 */
enum class TtsProvider : uint8_t {
    XFYUN = 0, /*!< 科大讯飞 WebSocket */
    BAIDU,     /*!< 百度 HTTP（待实现） */
    ALI,       /*!< 阿里 HTTP（待实现） */
};

/**
 * PCM 回调：厂商无关，解码出的 16-bit 单声道 PCM 都走这里
 */
using TtsPcmCb = void (*)(const int16_t *pcm, size_t samples,
                          uint32_t sampleRateHz, void *ctx);

/**
 * PCM 输出槽：板级把「已重采样好」的 PCM 推给功放的函数注册进来，
 * TtsHub 只管合成/重采样，不依赖具体功放驱动。
 */
using TtsPcmSink = void (*)(const int16_t *pcm, size_t bytes);

/**
 * 驱动接口表 —— 每个厂商实现一套填进来
 */
struct TtsDriver {
    const char *name = nullptr;
    esp_err_t (*init)(void) = nullptr;
    esp_err_t (*speak)(const char *text, TtsPcmCb cb, void *ctx) = nullptr;
    bool (*isBusy)(void) = nullptr;
};

/**
 * TTS 中心静态类
 *
 * 用法：
 *   TtsHub::init(boardTtsSink, 22050);       // 注入输出槽与喇叭采样率
 *   TtsHub::say(TtsPhrase::NET_CONNECTED);   // 固定话术（离线优先）
 *   TtsHub::speak("任意文本");                // 云 TTS（需联网）
 */
class TtsHub {
public:
    static constexpr uint32_t SRC_HZ = 16000;       /*!< 云 TTS / 离线片段统一输入采样率 */
    static constexpr size_t SEG_MAX_BYTES = 1200;   /*!< 单次云合成最大文本字节数（约 400 汉字/段） */

    /**
     * @brief 初始化 TTS 中心（把输出槽与喇叭采样率一并注入）
     *
     * @param sink           PCM 输出槽（板级功放适配器）
     * @param outputRateHz   喇叭采样率；云 TTS/片段统一 16k 输入，重采样到此值
     */
    static esp_err_t init(TtsPcmSink sink, uint32_t outputRateHz);

    /* ============ 任意文本（走云 TTS，需联网） ============ */
    static esp_err_t speak(const char *text);
    static esp_err_t speakAsync(const char *text);

    /* ============ 固定话术（离线优先，缺失自动回退云） ============ */
    static esp_err_t say(TtsPhrase ph);
    static esp_err_t sayAsync(TtsPhrase ph);

    /** 在线状态（上层在 GOT_IP / DISCONNECTED 时更新），影响固定话术兜底 */
    static void setOnline(bool online);
    static bool online();

    /** 该话术是否已内置离线片段（供上层决定「用片段还是本地提示音」） */
    static bool hasClip(TtsPhrase ph);

    static bool isBusy();
    static const char *providerName();

private:
    static const TtsDriver *activeDriver();
    static void playPcm16(const int16_t *in, size_t samples);
    static void cloudPlayCb(const int16_t *in, size_t samples, uint32_t srcHz, void *ctx);
    static size_t nextSegmentLen(const char *s, size_t total, size_t maxb);
    static size_t u8Len(unsigned char c);
    static size_t punctLen(const char *p);
    static void asyncTask(void *arg);

    static TtsPcmSink s_sink;
    static uint32_t s_outRate;
    static volatile bool s_online;
};

#endif /* TTS_HUB_H */
