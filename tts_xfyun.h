/**
 * @file     tts_xfyun.h
 * @brief    科大讯飞 WebSocket TTS 驱动 — C++ class
 *
 * 功能：wss://tts-api.xfyun.cn/v2/tts 语音合成，aue=raw 16-bit PCM 16kHz，
 *       HMAC-SHA256 鉴权（经 xfyun_auth 组件），流式 JSON 状态机解析
 *       （堆占用恒定几 KB，抗堆碎片）。
 * 修改：2026-11-21 由 C 驱动实例（tts_drv_xfyun）重构为 TtsXfyun 类；
 *       鉴权改经 XfyunAuth class；新增运行时凭据注入 setCredentials
 *       （优先于 tts_config.h 编译期宏，便于免重编译换 Key）
 */
#ifndef TTS_XFYUN_H
#define TTS_XFYUN_H

#include "tts_hub.h"

/**
 * 科大讯飞 TTS 驱动
 *
 * 由 TtsHub 内部经 driver() 引用，一般无需直接调用。
 */
class TtsXfyun {
public:
    /** 获取驱动实例（填进厂商表） */
    static const TtsDriver &driver();

    /**
     * @brief 运行时注入鉴权凭据（新功能）
     *
     * 设置后优先于 tts_config.h 的编译期宏（CONFIG_TTS_XFYUN_*）；
     * 任一参数为空则整体忽略。
     */
    static void setCredentials(const char *appId, const char *apiKey, const char *apiSecret);

    /** 生效凭据（运行时注入优先，否则编译期宏） */
    static const char *effAppId();
    static const char *effApiKey();
    static const char *effApiSecret();

private:
    static esp_err_t init();
    static esp_err_t speak(const char *text, TtsPcmCb cb, void *ctx);
    static bool isBusy();

    static char s_appId[32];
    static char s_apiKey[64];
    static char s_apiSecret[64];
    static bool s_credsSet;
};

#endif /* TTS_XFYUN_H */
