/**
 * @file     tts_phrases.h
 * @brief    TTS 固定话术表（离线片段 + 文案）— C++ class
 *
 * 功能：固定话术枚举、离线语音片段表（由 tools/gen_tts_clips.py 生成）、
 *       话术中文文案（供云 TTS 兜底 / 日志）。
 * 修改：2026-11-21 由 C API（tts_phrase_text + tts_clips 数组）重构为
 *       TtsPhrases 静态类
 */
#ifndef TTS_PHRASES_H
#define TTS_PHRASES_H

#include <stdint.h>

/**
 * 固定话术枚举 —— 新增一条只需在末尾（MAX 之前）加，并同步 gen 脚本
 */
enum class TtsPhrase : uint8_t {
    PORTAL_START = 0,   /*!< 配网中，请连接设备热点 */
    PORTAL_OK,          /*!< 配网成功 */
    NET_CONNECTED,      /*!< 网络已连接 */
    STARTUP,            /*!< 小智已启动 */
    NET_DISCONNECTED,   /*!< 网络已断开 */
    CONNECT_FAILED,     /*!< 连接失败，请检查密码 */
    PLEASE_WAIT,        /*!< 请稍候 */
    OK,                 /*!< 好的 */
    CANCEL,             /*!< 已取消 */
    PROCESSING,         /*!< 正在处理 */
    VOLUME_UP,          /*!< 音量已增大 */
    VOLUME_DOWN,        /*!< 音量已减小 */
    WAKE_REPLY,         /*!< 我在，请说 */
    MAX
};

/**
 * 一条离线语音片段：原始 s16le 单声道 PCM
 */
struct TtsClip {
    const uint8_t *data = nullptr; /*!< PCM 数据，nullptr 表示未提供 */
    uint32_t len = 0;              /*!< 字节数 */
    uint32_t sampleRate = 16000;   /*!< 采样率 Hz */
};

/**
 * 固定话术静态类
 */
class TtsPhrases {
public:
    /** 话术总数 */
    static constexpr int COUNT = static_cast<int>(TtsPhrase::MAX);

    /** 离线片段表（由 tools/gen_tts_clips.py 生成，当前为占位全 nullptr => 自动回退云 TTS） */
    static const TtsClip clips[COUNT];

    /** 该话术对应的中文文案（供云 TTS 兜底 / 日志） */
    static const char *text(TtsPhrase ph);
};

#endif /* TTS_PHRASES_H */
