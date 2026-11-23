/**
 * @file     tts_config.h
 * @brief    TTS 组件配置（PlatformIO 版：经 build_flags 覆盖，无 Kconfig）
 *
 * 修改：2026-11-21 适配 C++ class 重构：厂商枚举改用 TtsProvider；
 *       原 Kconfig.projbuild 移除，默认值在此定义，
 *       可在 platformio.ini 的 build_flags 覆盖，例如：
 *         -DCONFIG_TTS_XFYUN_APPID=\"xxx\"
 *         -DCONFIG_TTS_XFYUN_SPEED=60
 *       也可运行时经 TtsXfyun::setCredentials() 注入凭据（优先于宏）
 */
#ifndef TTS_CONFIG_H
#define TTS_CONFIG_H

#include "tts_hub.h"

/* 当前使用的 TTS 厂商（新增厂商时改这里） */
#ifndef TTS_ACTIVE_PROVIDER
#define TTS_ACTIVE_PROVIDER TtsProvider::XFYUN
#endif

/* ============ 科大讯飞配置 ============ */

/* 鉴权凭据（强烈建议运行时经 TtsXfyun::setCredentials 注入，避免进版本库） */
#ifndef CONFIG_TTS_XFYUN_APPID
#define CONFIG_TTS_XFYUN_APPID ""
#endif
#ifndef CONFIG_TTS_XFYUN_API_KEY
#define CONFIG_TTS_XFYUN_API_KEY ""
#endif
#ifndef CONFIG_TTS_XFYUN_API_SECRET
#define CONFIG_TTS_XFYUN_API_SECRET ""
#endif

/* 发音人：默认 xiaoyan（女声）；可选 xiaoyu / aisjiuxu / aisbabyxu 等 */
#ifndef CONFIG_TTS_XFYUN_VCN
#define CONFIG_TTS_XFYUN_VCN "xiaoyan"
#endif

/* 语速 0-100，默认 40（原 Kconfig 默认） */
#ifndef CONFIG_TTS_XFYUN_SPEED
#define CONFIG_TTS_XFYUN_SPEED 40
#endif

/* 音量 0-100，默认 50（原 Kconfig 默认） */
#ifndef CONFIG_TTS_XFYUN_VOLUME
#define CONFIG_TTS_XFYUN_VOLUME 50
#endif

/* 供驱动使用的短名 */
#define TTS_XFYUN_APPID      CONFIG_TTS_XFYUN_APPID
#define TTS_XFYUN_API_KEY    CONFIG_TTS_XFYUN_API_KEY
#define TTS_XFYUN_API_SECRET CONFIG_TTS_XFYUN_API_SECRET
#define TTS_XFYUN_VCN        CONFIG_TTS_XFYUN_VCN
#define TTS_XFYUN_SPEED      CONFIG_TTS_XFYUN_SPEED
#define TTS_XFYUN_VOLUME     CONFIG_TTS_XFYUN_VOLUME

#endif /* TTS_CONFIG_H */
