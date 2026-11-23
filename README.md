# TTS Hub 组件 v1.0（C++ class 版）

## 一、组件概述

TTS Hub 是厂商无关的统一语音合成中心：任意文本走云 TTS（需联网），固定话术离线片段优先、缺失自动回退云 TTS；收到 16k PCM 后线性重采样到喇叭采样率再送输出槽。

### 特性

- **统一入口**：`TtsHub::speak(任意文本)` / `TtsHub::say(固定话术)`，同步与异步两套
- **厂商驱动表**：当前科大讯飞（WebSocket），百度/阿里预留，新增厂商只需实现 `TtsDriver` 加一行
- **离线优先**：固定话术有内置片段直接播（断网可用），无片段在线回退云 TTS
- **长文本分段**：按中文标点/UTF-8 边界自动切段（单段 ≤1200 字节），逐段合成播放
- **流式解析**：讯飞报文逐字节状态机解析 + base64 边收边解，堆占用恒定几 KB（抗堆碎片）
- **重采样**：16k 输入线性重采样到板级喇叭采样率，采样率一致时零开销直通
- **凭据注入**：编译期宏（build_flags）或运行时 `TtsXfyun::setCredentials()`（优先）

---

## 二、目录结构

```
tts_hub/
├── tts_hub.h / tts_hub.cpp          # TtsHub 静态类（统一入口 + 重采样 + 分段）
├── tts_phrases.h / tts_phrases.cpp  # TtsPhrases 固定话术表（gen 脚本生成）
├── tts_xfyun.h / tts_xfyun.cpp      # TtsXfyun 科大讯飞 WebSocket 驱动
├── tts_config.h                     # 配置（build_flags 覆盖，无 Kconfig）
├── tools/
│   └── gen_tts_clips.py             # 离线话术片段生成脚本
├── library.json                     # PlatformIO 库描述
└── README.md                        # 开发文档（本文件）
```

---

## 三、快速上手

```cpp
#include "tts_hub.h"
#include "tts_xfyun.h"

/* 板级 PCM 输出槽：把重采样好的 PCM 推给功放 */
static void boardTtsSink(const int16_t *pcm, size_t bytes) {
    // i2s_write(...) / 功放适配
}

void app_main() {
    /* 可选：运行时注入讯飞凭据（优先于 build_flags 宏） */
    TtsXfyun::setCredentials("appid", "apikey", "apisecret");

    TtsHub::init(boardTtsSink, 22050);   // 喇叭采样率

    /* 联网状态同步（影响固定话术兜底策略） */
    TtsHub::setOnline(true);

    TtsHub::say(TtsPhrase::NET_CONNECTED);      // 固定话术（离线优先）
    TtsHub::speak("今天天气不错，适合出门。");    // 任意文本（需联网）
    TtsHub::sayAsync(TtsPhrase::STARTUP);       // 异步版（内部起任务）
}
```

---

## 四、API 概览

### 4.1 TtsHub（静态类）

| 方法 | 说明 |
|------|------|
| `init(sink, outputRateHz)` | 注入输出槽与喇叭采样率，初始化当前厂商驱动 |
| `speak(text) / speakAsync(text)` | 任意文本合成（云 TTS，需联网；长文本自动分段） |
| `say(ph) / sayAsync(ph)` | 固定话术（离线片段优先，缺失回退云） |
| `setOnline(b) / online()` | 在线状态（上层在联网事件中维护） |
| `hasClip(ph)` | 该话术是否有内置离线片段 |
| `isBusy()` | 是否正在合成 |
| `providerName()` | 当前厂商名 |

### 4.2 固定话术 `TtsPhrase`

PORTAL_START / PORTAL_OK / NET_CONNECTED / STARTUP / NET_DISCONNECTED /
CONNECT_FAILED / PLEASE_WAIT / OK / CANCEL / PROCESSING / VOLUME_UP /
VOLUME_DOWN / WAKE_REPLY

> 离线片段当前为占位（全 nullptr，自动回退云 TTS）。
> 生成真实片段：`python tools/gen_tts_clips.py`（依赖 edge-tts + ffmpeg）。

### 4.3 TtsXfyun（讯飞驱动）

| 方法 | 说明 |
|------|------|
| `driver()` | 驱动实例（TtsHub 内部引用） |
| `setCredentials(appId, apiKey, apiSecret)` | 运行时凭据注入（新功能，优先于宏） |

### 4.4 配置（tts_config.h，build_flags 覆盖）

| 宏 | 默认 | 说明 |
|----|------|------|
| `TTS_ACTIVE_PROVIDER` | `TtsProvider::XFYUN` | 当前厂商 |
| `CONFIG_TTS_XFYUN_APPID/API_KEY/API_SECRET` | 空 | 鉴权凭据 |
| `CONFIG_TTS_XFYUN_VCN` | xiaoyan | 发音人 |
| `CONFIG_TTS_XFYUN_SPEED` | 40 | 语速 0-100 |
| `CONFIG_TTS_XFYUN_VOLUME` | 50 | 音量 0-100 |

---

## 五、技术说明（本次重构要点）

### 设计要点（沿用原实现的关键决策）

- **流式 JSON 状态机**：ESP32-S3 连 WiFi 后剩余堆碎片化，拿不到 40KB 连续块，整帧缓冲 + cJSON 方案不可行；逐字节解析堆占用恒定
- **分片不能重置状态**：`payload_len` 是当前分片长度而非整条消息长度，据此判断 last 会清零正在累积的 base64
- **文本帧在任务上下文发送**：ws 事件回调内发送会与客户端锁死锁（"Could not lock ws-client"）
- **job 结构放静态区**：含 3KB base64 缓冲，8KB 任务栈放不下（实测栈溢出）
- **SNTP 等待**：鉴权 GMT 时间依赖对时，`XfyunAuth::waitSntp(20)` 最多等 20 秒
- **重采样**：16k→喇叭采样率线性插值；采样率一致直通零开销

### 新增功能

- `TtsXfyun::setCredentials()` 运行时凭据注入（免重编译换 Key）

### 其他变更

- C API（`tts_hub_*` / `tts_phrase_text` / `tts_drv_xfyun`）重构为 C++ class，不保留兼容层
- 鉴权改经 `XfyunAuth` class（xfyun_auth 组件）
- 移除 Kconfig.projbuild（PlatformIO 用 build_flags）、idf_component.yml（依赖移入 library.json）

---

## 六、版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0.0 | 2026-11-21 | 重构为 C++ class（TtsHub/TtsPhrases/TtsXfyun）；对接 XfyunAuth class；新增运行时凭据注入；Kconfig 改 build_flags |
| - | - | ESP-IDF C 版本（tts_hub_* 函数 API） |

## 许可证

MIT License
