/**
 * @file     tts_xfyun.cpp
 * @brief    科大讯飞 WebSocket TTS 驱动实现
 *
 * 鉴权：HMAC-SHA256 + GMT 时间，wss://tts-api.xfyun.cn/v2/tts
 * 音频：aue=raw（16-bit PCM 16000Hz），经回调原样上抛，
 *       重采样由上层（播放侧）按喇叭采样率统一处理。
 *
 * 接收侧采用逐字节流式解析（见下方状态机）：堆占用恒定几 KB。
 * 原因：实测 ESP32-S3 连上 WiFi 后剩余堆虽有 80KB+，但碎片化后拿不到
 * 40KB 连续块（剩余堆 79156 时分配 43633 仍失败），整帧缓冲方案不可行；
 * 且 cJSON 解析树还会再拷一份同样大小的字符串，峰值直接翻倍。
 *
 * 修改：2026-11-21 重构为 TtsXfyun 类；鉴权改经 XfyunAuth；
 *       新增运行时凭据注入
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "tts_hub.h"
#include "tts_config.h"
#include "tts_xfyun.h"
#include "xfyun_auth.h"

static const char *TAG = "TTS_XFYUN";

#define XF_HOST     "tts-api.xfyun.cn"
#define XF_PATH     "/v2/tts"

/* ============ 流式 JSON 解析状态机 ============
 * 讯飞报文结构固定：
 *   {"code":0,"message":"success","sid":"...","data":{"audio":"<base64>","ced":"9","status":2}}
 * 逐字节扫描字段，天然跨 WebSocket 分片连续，全程无需大缓冲。
 */
enum class XfState : uint8_t {
    SCAN = 0,  /* 找下一个 '"' */
    KEY,       /* 读字段名 */
    COLON,     /* 等 ':' */
    VSTART,    /* 值起始：引号进字符串，数字进数值 */
    STRVAL,    /* 读字符串值（audio 走 base64 累积） */
    NUMVAL,    /* 读数值直到 , } ] */
};

#define XF_MARK_CODE   (1u << 0)
#define XF_MARK_MSG    (1u << 1)
#define XF_MARK_AUDIO  (1u << 2)
#define XF_MARK_STATUS (1u << 3)

#define XF_B64_BUF     3072   /* base64 累积（4 的倍数，可解出 2304 字节 PCM） */
#define XF_MSG_LEN     96

struct XfyunJob {
    const char *text;   /* 待合成 UTF-8 文本 */
    TtsPcmCb cb;
    void *uctx;         /* 回调用户上下文 */
    SemaphoreHandle_t done;
    SemaphoreHandle_t connected;  /* ws 已连接信号：发文本挪到任务里做，避免回调内重入锁死 */
    volatile bool failed;
    esp_websocket_client_handle_t cli;

    /* ---- 流式解析状态，无大缓冲 ---- */
    XfState xs;
    size_t mark;            /* 当前字段对应的 XF_MARK_* */
    bool esc;               /* 字符串转义 */
    bool inAudio;           /* 当前字符串是 audio */
    char key[16];
    size_t keyLen;
    char val[24];           /* code / status 的数字文本 */
    size_t valLen;
    char msg[XF_MSG_LEN];   /* 错误 message（截断保存） */
    size_t msgLen;
    int code;
    bool hasCode;
    int status;
    uint8_t b64[XF_B64_BUF];
    size_t b64Len;
    bool frameDone;         /* 本帧已见到对象结尾 */
};

static volatile bool s_busy = false;

/* 诊断用：本句解出多少批/多少字节 PCM（排查「有日志无声音」） */
static uint32_t s_audioBatches;
static uint32_t s_audioBytes;

static void audioFeed(XfyunJob *job, char ch);

static void audioReset(XfyunJob *job)
{
    job->xs        = XfState::SCAN;
    job->mark      = 0;
    job->esc       = false;
    job->inAudio   = false;
    job->keyLen    = 0;
    job->valLen    = 0;
    job->msgLen    = 0;
    job->code      = 0;
    job->hasCode   = false;
    job->status    = 0;
    job->b64Len    = 0;
    job->frameDone = false;
}

static void markApply(XfyunJob *job, const char *key)
{
    if      (strcmp(key, "code") == 0)    job->mark = XF_MARK_CODE;
    else if (strcmp(key, "message") == 0) job->mark = XF_MARK_MSG;
    else if (strcmp(key, "audio") == 0)   job->mark = XF_MARK_AUDIO;
    else if (strcmp(key, "status") == 0)  job->mark = XF_MARK_STATUS;
    else                                  job->mark = 0;
}

/* 字段值结束（字符串与数值都走这里） */
static void fieldEnd(XfyunJob *job)
{
    job->val[job->valLen] = '\0';
    switch (job->mark) {
    case XF_MARK_CODE:
        job->code    = atoi(job->val);
        job->hasCode = true;
        break;
    case XF_MARK_MSG: {
        size_t n = job->valLen;
        if (n > XF_MSG_LEN - 1) n = XF_MSG_LEN - 1;
        memcpy(job->msg, job->val, n);
        job->msg[n]  = '\0';
        job->msgLen  = n;
        break;
    }
    case XF_MARK_STATUS:
        job->status = atoi(job->val);
        break;
    default:
        break;
    }
}

/* 把一个 WebSocket 数据分片喂进状态机 */
static void xfFeed(XfyunJob *job, const char *p, size_t len)
{
    /* 讯飞 TTS 按多条报文推流（每条含一段音频）。上一条报文解析完时
     * frameDone 会置真，若不复位，后续报文会被整体忽略 —— 表现为
     * 只播第一段音频、长句/"连接成功"等尾段全丢。新分片到来若已结束，
     * 就复位解析状态，把本次数据当作新报文继续解析。 */
    if (job->frameDone) {
        job->frameDone = false;
        job->xs = XfState::SCAN;
    }

    for (size_t i = 0; i < len && !job->frameDone; i++) {
        char ch = p[i];
        switch (job->xs) {
        case XfState::SCAN:
            if (ch == '"') { job->xs = XfState::KEY; job->keyLen = 0; }
            break;

        case XfState::KEY:
            if (ch == '"') {
                job->key[job->keyLen] = '\0';
                job->xs = XfState::COLON;
            } else if (job->keyLen < sizeof(job->key) - 1) {
                job->key[job->keyLen++] = ch;
            }
            break;

        case XfState::COLON:
            if (ch == ':') {
                markApply(job, job->key);
                job->xs      = XfState::VSTART;
                job->valLen  = 0;
                job->inAudio = (job->mark == XF_MARK_AUDIO);
            }
            break;

        case XfState::VSTART:
            if (ch == '{') {
                /* 进入嵌套对象（如 data）。其内部的键名（如 "audio"）不是字段值，
                 * 直接把状态交回 SCAN，让它按「下一个字段」正常扫描，
                 * 避免把键名误当字符串值吃掉造成错位。 */
                job->xs = XfState::SCAN;
            } else if (ch == '"') {
                job->xs     = XfState::STRVAL;
                job->esc    = false;
                job->valLen = 0;
                if (job->inAudio) job->b64Len = 0;
            } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                job->xs     = XfState::NUMVAL;
                job->valLen = 0;
                if (job->valLen < sizeof(job->val) - 1) job->val[job->valLen++] = ch;
            } else if (ch == ',' || ch == '}' || ch == ']') {
                job->xs = XfState::SCAN;   /* 空值 */
            }
            break;

        case XfState::STRVAL:
            if (job->esc) {
                job->esc = false;
                if (job->inAudio) audioFeed(job, ch);
            } else if (ch == '\\') {
                job->esc = true;
            } else if (ch == '"') {
                if (job->inAudio) audioFeed(job, '\0');  /* 冲刷 base64 尾部 */
                else              fieldEnd(job);
                job->xs = XfState::SCAN;
            } else if (job->inAudio) {
                audioFeed(job, ch);
            } else if (job->valLen < sizeof(job->val) - 1) {
                job->val[job->valLen++] = ch;
            }
            break;

        case XfState::NUMVAL:
            if ((ch >= '0' && ch <= '9') || ch == '-') {
                if (job->valLen < sizeof(job->val) - 1) job->val[job->valLen++] = ch;
            } else {
                fieldEnd(job);
                job->xs = XfState::SCAN;
                if (ch == '}') job->frameDone = true;
            }
            break;

        default:
            break;
        }
    }
}

/* 收到的 base64 字符喂进解码器：攒够 4 的倍数就解一批 PCM 送播放。
 * 缓冲固定 3KB 且反复复用；ch == 0 表示字符串结束，冲刷残余。 */
static void audioFeed(XfyunJob *job, char ch)
{
    if (ch == '=') return;    /* base64 padding，无需入缓冲 */

    if (ch != '\0') {
        job->b64[job->b64Len++] = (uint8_t)ch;
        if (job->b64Len < XF_B64_BUF) return;
    }

    size_t usable = job->b64Len & ~(size_t)3;   /* 4 字符 -> 3 字节 */
    if (usable && job->cb) {
        size_t need = usable / 4 * 3;
        size_t olen = 0;
        /* 原地解码：输出比输入短（3/4），前向覆盖安全 */
        if (mbedtls_base64_decode(job->b64, need, &olen, job->b64, usable) == 0
            && olen >= 2) {
            s_audioBatches++;
            s_audioBytes += olen;
            job->cb((int16_t *)job->b64, olen / 2, 16000, job->uctx);
        }
    }
    if (ch == '\0') {
        job->b64Len = 0;
        return;
    }
    size_t rem = job->b64Len - usable;          /* 保留 0~3 个残余字符 */
    if (rem) memmove(job->b64, job->b64 + usable, rem);
    job->b64Len = rem;
}

/* ============ 凭据（运行时注入优先于编译期宏） ============ */

char TtsXfyun::s_appId[32] = {0};
char TtsXfyun::s_apiKey[64] = {0};
char TtsXfyun::s_apiSecret[64] = {0};
bool TtsXfyun::s_credsSet = false;

void TtsXfyun::setCredentials(const char *appId, const char *apiKey, const char *apiSecret)
{
    if (appId == nullptr || apiKey == nullptr || apiSecret == nullptr) return;
    strncpy(s_appId, appId, sizeof(s_appId) - 1);
    s_appId[sizeof(s_appId) - 1] = '\0';
    strncpy(s_apiKey, apiKey, sizeof(s_apiKey) - 1);
    s_apiKey[sizeof(s_apiKey) - 1] = '\0';
    strncpy(s_apiSecret, apiSecret, sizeof(s_apiSecret) - 1);
    s_apiSecret[sizeof(s_apiSecret) - 1] = '\0';
    s_credsSet = true;
}

const char *TtsXfyun::effAppId()     { return s_credsSet ? s_appId : TTS_XFYUN_APPID; }
const char *TtsXfyun::effApiKey()    { return s_credsSet ? s_apiKey : TTS_XFYUN_API_KEY; }
const char *TtsXfyun::effApiSecret() { return s_credsSet ? s_apiSecret : TTS_XFYUN_API_SECRET; }

/* ============ 鉴权 ============ */

/* 鉴权统一走 xfyun_auth 组件（与语音听写引擎共用一份实现）。
 * URL 缓冲较大，放静态区：调用链在 speak 任务栈上。 */
static char s_url[2600];      /* websocket URI 必须活到 client_destroy */

static bool buildAuthUrl(char *out, size_t outN)
{
    XfyunAuth auth;
    auth.setCredentials({ TtsXfyun::effApiKey(), TtsXfyun::effApiSecret() });
    return auth.buildAuthUrl(XF_HOST, XF_PATH, out, outN) == ESP_OK;
}

/* ============ WebSocket 事件 ============ */

/* 在 speak 任务上下文里发送文本帧。
 * 注意：绝不能放在 ws 事件回调里发——回调运行于 ws 客户端任务且持有客户端锁，
 * 回调内再调 esp_websocket_client_send_text 会抢同一把锁而死锁
 * （日志 "Could not lock ws-client within 20 timeout"，合成卡 20~30s）。 */
static void sendTextFrame(XfyunJob *job)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON_AddStringToObject(common, "app_id", TtsXfyun::effAppId());
    cJSON_AddItemToObject(root, "common", common);

    cJSON *bus = cJSON_CreateObject();
    cJSON_AddStringToObject(bus, "aue", "raw");
    cJSON_AddStringToObject(bus, "auf", "audio/L16;rate=16000");
    cJSON_AddStringToObject(bus, "vcn", TTS_XFYUN_VCN);
    cJSON_AddStringToObject(bus, "tte", "UTF8");
    cJSON_AddNumberToObject(bus, "speed", TTS_XFYUN_SPEED);
    cJSON_AddNumberToObject(bus, "volume", TTS_XFYUN_VOLUME);
    cJSON_AddItemToObject(root, "business", bus);

    /* 文本需先做 UTF-8 Base64（讯飞要求） */
    size_t tlen = strlen(job->text);
    size_t blen = (tlen + 2) / 3 * 4 + 1;
    char *b64 = (char *)malloc(blen);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)b64, blen, &olen,
                          (const unsigned char *)job->text, tlen);
    cJSON *dt = cJSON_CreateObject();
    cJSON_AddNumberToObject(dt, "status", 2);
    cJSON_AddStringToObject(dt, "text", b64);
    free(b64);
    cJSON_AddItemToObject(root, "data", dt);

    char *js = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "发送文本帧");
    esp_websocket_client_send_text(job->cli, js, strlen(js), portMAX_DELAY);
    cJSON_free(js);
    cJSON_Delete(root);
}

static void wsEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    XfyunJob *job = (XfyunJob *)arg;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    (void)base;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        /* 只置信号，真正的发送交给 speak 任务（见 sendTextFrame 注释） */
        ESP_LOGI(TAG, "ws 已连接");
        xSemaphoreGive(job->connected);
        break;

    case WEBSOCKET_EVENT_DATA: {
        /* 分片逐片喂状态机：堆占用恒定，无需按 payload_len 缓冲整帧。
         *
         * 关键：这里绝不能按「分片结束」重置状态。esp_websocket 客户端的
         * payload_len 是当前分片长度而非整条消息长度，据此判断 last 会导致
         * 每个分片都重置，把正在累积的 base64 清零 —— 结果是一帧音频都攒
         * 不出来（表现为「解码 0 字节」但末尾 status=2 仍能收到）。
         * 状态机本身跨分片连续，只在整句合成结束时才复位。 */
        xfFeed(job, ev->data_ptr, ev->data_len);

        if (job->hasCode && job->code != 0) {
            ESP_LOGE(TAG, "错误 code=%d msg=%s", job->code,
                     job->msgLen ? job->msg : "(无 message 字段)");
            job->failed = true;
            audioReset(job);
            xSemaphoreGive(job->done);
            break;
        }
        if (job->status == 2) {        /* 合成结束：此时才复位 */
            ESP_LOGI(TAG, "合成结束，累计解码 %u 批 / %u 字节 PCM",
                     (unsigned)s_audioBatches, (unsigned)s_audioBytes);
            audioReset(job);
            xSemaphoreGive(job->done);
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_DISCONNECTED:
        job->failed = true;
        xSemaphoreGive(job->connected);   /* 唤醒等待连接的任务，避免干等 */
        xSemaphoreGive(job->done);
        break;
    default:
        break;
    }
}

/* job 含 3KB 的 base64 累积缓冲，绝不能放任务栈：
 * speak 任务栈仅 8KB，叠加 url(2.6KB) 与 buildAuthUrl 的局部缓冲
 * 会超过 9.7KB，实测直接栈溢出（LoadStoreError，EXCVADDR=0）。
 * 单实例（s_busy 互斥），用静态区最合适。 */
static XfyunJob s_job;

esp_err_t TtsXfyun::init()
{
    return ESP_OK;
}

bool TtsXfyun::isBusy()
{
    return s_busy;
}

esp_err_t TtsXfyun::speak(const char *text, TtsPcmCb cb, void *ctx)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (text == nullptr || text[0] == '\0') return ESP_ERR_INVALID_ARG;
    s_busy = true;

    /* 等 SNTP 对时，否则 GMT 鉴权必失败（握手 403）。
     * 讯飞允许 300 秒时钟偏差，但首次联网 NTP 常需 5~15s。 */
    XfyunAuth::waitSntp(20);

    XfyunJob *job = &s_job;
    memset(job, 0, sizeof(*job));
    job->text = text;
    job->cb   = cb;
    job->uctx = ctx;
    job->done = xSemaphoreCreateBinary();
    job->connected = xSemaphoreCreateBinary();
    audioReset(job);
    s_audioBatches = 0;
    s_audioBytes   = 0;

    if (!buildAuthUrl(s_url, sizeof(s_url))) {
        ESP_LOGE(TAG, "鉴权 URL 生成失败，检查 API Key/Secret 配置");
        s_busy = false;
        return ESP_FAIL;
    }

    /* TLS 会话要一大块连续内存，碎片化时握手会失败（PSA -0x8D）。
     * 同时打印内部 RAM（TLS/PSA 走内部），便于定位 HTTP 之后 TTS 卡顿。 */
    ESP_LOGI(TAG, "建连前堆: 剩余=%u 最大连续块=%u | 内部剩=%u 内部最大块=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        /* 8KB：一帧音频 JSON 会被切片，由流式状态机跨片处理 */
        .buffer_size = 8192,
        .task_stack = 8192,
        .disable_auto_reconnect = true,
        /* 三遍/长句音频边合成边到达，默认 10s 易被中途超时打断，放宽到 20s */
        .network_timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* wss 必须挂证书 */
    };
    job->cli = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(job->cli, WEBSOCKET_EVENT_ANY, wsEvent, job);
    esp_websocket_client_start(job->cli);

    /* 等连接后，在本任务上下文发文本帧，再等合成结束 */
    bool ok = false;
    if (xSemaphoreTake(job->connected, pdMS_TO_TICKS(15000)) == pdTRUE && !job->failed) {
        sendTextFrame(job);
        ok = (xSemaphoreTake(job->done, pdMS_TO_TICKS(30000)) == pdTRUE)
             && !job->failed;
    }
    esp_websocket_client_stop(job->cli);
    esp_websocket_client_destroy(job->cli);
    vSemaphoreDelete(job->done);
    vSemaphoreDelete(job->connected);
    job->cli = nullptr;
    ESP_LOGI(TAG, "音频解码: %u 批 / %u 字节 PCM (16kHz)",
             (unsigned)s_audioBatches, (unsigned)s_audioBytes);
    s_busy = false;
    return ok ? ESP_OK : ESP_FAIL;
}

const TtsDriver &TtsXfyun::driver()
{
    static const TtsDriver drv = {
        .name   = "xfyun",
        .init   = TtsXfyun::init,
        .speak  = TtsXfyun::speak,
        .isBusy = TtsXfyun::isBusy,
    };
    return drv;
}
