/**
 * @file     tts_phrases.cpp
 * @brief    固定话术数据表
 *
 * 本文件由 tools/gen_tts_clips.py 生成，请勿手改。
 * 当前为占位版本：所有片段为 nullptr，运行时自动回退到云 TTS。
 *
 * 生成真实片段：
 *   python tools/gen_tts_clips.py
 * 依赖：edge-tts + ffmpeg（或预先准备好的 wav 目录，见脚本说明）
 *
 * 修改：2026-11-21 重构为 TtsPhrases 静态类成员
 */
#include "tts_phrases.h"

#include <stddef.h>

const TtsClip TtsPhrases::clips[TtsPhrases::COUNT] = {
    /* PORTAL_START */     { nullptr, 0, 16000 },
    /* PORTAL_OK */        { nullptr, 0, 16000 },
    /* NET_CONNECTED */    { nullptr, 0, 16000 },
    /* STARTUP */          { nullptr, 0, 16000 },
    /* NET_DISCONNECTED */ { nullptr, 0, 16000 },
    /* CONNECT_FAILED */   { nullptr, 0, 16000 },
    /* PLEASE_WAIT */      { nullptr, 0, 16000 },
    /* OK */               { nullptr, 0, 16000 },
    /* CANCEL */           { nullptr, 0, 16000 },
    /* PROCESSING */       { nullptr, 0, 16000 },
    /* VOLUME_UP */        { nullptr, 0, 16000 },
    /* VOLUME_DOWN */      { nullptr, 0, 16000 },
    /* WAKE_REPLY */       { nullptr, 0, 16000 },
};

const char *TtsPhrases::text(TtsPhrase ph)
{
    switch (ph) {
    case TtsPhrase::PORTAL_START:     return "配网中，请连接设备热点";
    case TtsPhrase::PORTAL_OK:        return "配网成功";
    case TtsPhrase::NET_CONNECTED:    return "网络已连接";
    case TtsPhrase::STARTUP:          return "小智已启动";
    case TtsPhrase::NET_DISCONNECTED: return "网络已断开";
    case TtsPhrase::CONNECT_FAILED:   return "连接失败，请检查密码";
    case TtsPhrase::PLEASE_WAIT:      return "请稍候";
    case TtsPhrase::OK:               return "好的";
    case TtsPhrase::CANCEL:           return "已取消";
    case TtsPhrase::PROCESSING:       return "正在处理";
    case TtsPhrase::VOLUME_UP:        return "音量已增大";
    case TtsPhrase::VOLUME_DOWN:      return "音量已减小";
    case TtsPhrase::WAKE_REPLY:       return "我在，请说";
    default:                          return "";
    }
}
