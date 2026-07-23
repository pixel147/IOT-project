/* ============================================================
 * llm_client.c — 大模型客户端（espressif/openai 封装）
 *
 * 底层: espressif/openai 官方组件
 *   - OpenAICreate / OpenAIChangeBaseURL / chatCreate / message / clearConversation
 *   - esp_crt_bundle TLS 证书验证
 *
 * 本层增值:
 *   - 情绪感知系统提示词自动生成
 *   - 简化的同步 / 异步 C API + 互斥锁
 * ============================================================ */

#include "llm_client.h"
#include "OpenAI.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "LLM";

/* ============================================================
 * 情绪提示词（extern 声明在 llm_config.h）
 * ============================================================ */

/* extern const 确保外部链接（C++ const 默认为内部链接，加 extern 覆盖） */
extern const char *LLM_EMOTION_NAMES[];
const char *LLM_EMOTION_NAMES[] = {
    "生气", "厌恶", "害怕", "开心", "难过", "惊讶", "平静"
};

extern const char *LLM_EMOTION_PROMPTS[];
const char *LLM_EMOTION_PROMPTS[] = {
    "用户此刻感到愤怒。请用温和、安抚的语气回应，帮助 TA 平复情绪。",
    "用户此刻感到厌恶。请试着理解 TA 的感受，引导 TA 放宽心态。",
    "用户此刻感到害怕。请给 TA 安全感和鼓励，让 TA 感到安心。",
    "用户此刻心情愉悦。请与 TA 分享快乐，保持轻松愉快的对话节奏。",
    "用户此刻有些难过。请用温暖的话语安慰 TA，给 TA 情感支持。",
    "用户此刻感到惊讶。请与 TA 一起探索这个惊喜，保持有趣互动。",
    "用户此刻情绪平静。请保持自然轻松的对话，做 TA 的日常陪伴者。",
};

/* ============================================================
 * 默认系统提示词
 * ============================================================ */

static const char *DEFAULT_SYSTEM_PROMPT =
    "你是一个温暖、善解人意的情绪陪伴助手，名字叫「小守」。"
    "请用简洁自然的中文回复，像朋友聊天一样，每次回复控制在 2-4 句话。";

/* ============================================================
 * 内部常量
 * ============================================================ */

#define MAX_RESPONSE      4096     /* 回复缓冲                                   */
#define MAX_SYS_PROMPT    1536     /* 系统提示词缓冲（含情绪注入）                    */
#define ASYNC_STACK       8192     /* 异步任务栈                                  */
#define ASYNC_PRIO        2        /* 异步任务优先级                                */

/* ---- 异步请求上下文 ---- */
typedef struct {
    char                *message;
    llm_chat_callback_t  cb;
    void                *user_data;
} async_ctx_t;

/* ============================================================
 * 全局状态
 * ============================================================ */

static struct {
    OpenAI_t               *oai;       /* 来自 OpenAICreate()              */
    OpenAI_ChatCompletion_t *chat;     /* 来自 oai->chatCreate()           */
    SemaphoreHandle_t       mutex;

    /* 配置副本 */
    char   base_url[192];
    char   model[64];
    int    max_tokens;
    float  temperature;

    /* 系统提示词 */
    char        sys_buf[MAX_SYS_PROMPT];
    char       *custom_prompt;          /* 用户手动设置（堆分配）            */

    /* 情绪提供者回调（Pull 模式：每次 chat 前实时拉取） */
    llm_emotion_provider_t  emotion_provider;

    /* 统计 */
    int     turn_count;

    /* 异步 */
    volatile bool busy;
} s_ctx;

static inline void lock(void)   { if (s_ctx.mutex) xSemaphoreTake(s_ctx.mutex, portMAX_DELAY); }
static inline void unlock(void) { if (s_ctx.mutex) xSemaphoreGive(s_ctx.mutex); }

/* ============================================================
 * 内部: 重建系统提示词 → 写入 s_ctx.sys_buf → chat->setSystem()
 * ============================================================ */

static void rebuild_system_prompt(void)
{
    if (!s_ctx.chat) return;

    const char *base = s_ctx.custom_prompt
                     ? s_ctx.custom_prompt
                     : DEFAULT_SYSTEM_PROMPT;

    /* Pull 模式：通过回调实时拉取最新情绪 */
    bool has = false;
    int  cls = 6;
    float conf = 0.0f;
    if (s_ctx.emotion_provider) {
        s_ctx.emotion_provider(&cls, &conf);
        has = (cls >= 0 && cls <= 6);
    }

    if (has) {
        int pct = (int)(conf * 100.0f + 0.5f);
        if (pct > 100) pct = 100;
        snprintf(s_ctx.sys_buf, sizeof(s_ctx.sys_buf),
                 "%s\n\n[情绪感知] %s（置信度 %d%%）\n%s",
                 base,
                 LLM_EMOTION_NAMES[cls],
                 pct,
                 LLM_EMOTION_PROMPTS[cls]);
    } else {
        strncpy(s_ctx.sys_buf, base, sizeof(s_ctx.sys_buf) - 1);
        s_ctx.sys_buf[sizeof(s_ctx.sys_buf) - 1] = '\0';
    }

    s_ctx.chat->setSystem(s_ctx.chat, s_ctx.sys_buf);
}

/* ============================================================
 * 内部: 从 OpenAI_StringResponse_t 提取文本
 * ============================================================ */

static esp_err_t extract_response(OpenAI_StringResponse_t *resp,
                                  char *buf, size_t size)
{
    if (!resp || !buf || size == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    /* 检查 API 错误 */
    char *err = resp->getError(resp);
    if (err) {
        snprintf(buf, size, "[API Error] %s", err);
        ESP_LOGE(TAG, "%s", buf);
        return ESP_FAIL;
    }

    /* 正常提取 */
    if (resp->getLen(resp) >= 1) {
        char *text = resp->getData(resp, 0);
        if (text) {
            strncpy(buf, text, size - 1);
            buf[size - 1] = '\0';
            ESP_LOGI(TAG, "Reply (%" PRIu32 " tokens): %.80s…",
                     resp->getUsage(resp), buf);
            return ESP_OK;
        }
    }

    snprintf(buf, size, "[Empty response]");
    ESP_LOGW(TAG, "Empty response");
    return ESP_FAIL;
}

/* ============================================================
 * 公开 API
 * ============================================================ */

/* ---- 生命周期 ---- */

esp_err_t llm_init(const llm_config_t *config)
{
    if (!config || !config->base_url || !config->api_key || !config->model) {
        ESP_LOGE(TAG, "Invalid config (need base_url, api_key, model)");
        return ESP_ERR_INVALID_ARG;
    }

    /* 首次创建互斥锁 */
    if (!s_ctx.mutex) {
        s_ctx.mutex = xSemaphoreCreateMutex();
        if (!s_ctx.mutex) return ESP_ERR_NO_MEM;
    }

    lock();

    /* 如果已有实例，先释放 */
    if (s_ctx.chat) { s_ctx.oai->chatDelete(s_ctx.chat); s_ctx.chat = NULL; }
    if (s_ctx.oai)  { OpenAIDelete(s_ctx.oai);           s_ctx.oai  = NULL; }

    /* 创建 OpenAI 实例 + 切换 Base URL */
    s_ctx.oai = OpenAICreate(config->api_key);
    if (!s_ctx.oai) {
        ESP_LOGE(TAG, "OpenAICreate failed");
        unlock();
        return ESP_FAIL;
    }

    OpenAIChangeBaseURL(s_ctx.oai, config->base_url);

    /* 创建 ChatCompletion 实例 */
    s_ctx.chat = s_ctx.oai->chatCreate(s_ctx.oai);
    if (!s_ctx.chat) {
        ESP_LOGE(TAG, "chatCreate failed");
        OpenAIDelete(s_ctx.oai);
        s_ctx.oai = NULL;
        unlock();
        return ESP_FAIL;
    }

    /* 保存配置 */
    strncpy(s_ctx.base_url, config->base_url, sizeof(s_ctx.base_url) - 1);
    strncpy(s_ctx.model,    config->model,    sizeof(s_ctx.model) - 1);
    s_ctx.max_tokens  = config->max_tokens  > 0 ? config->max_tokens  : LLM_DEFAULT_MAX_TOKENS;
    s_ctx.temperature = config->temperature > 0 ? config->temperature : LLM_DEFAULT_TEMPERATURE;

    /* 配置模型参数 */
    s_ctx.chat->setModel(s_ctx.chat, s_ctx.model);
    s_ctx.chat->setMaxTokens(s_ctx.chat, s_ctx.max_tokens);
    s_ctx.chat->setTemperature(s_ctx.chat, s_ctx.temperature);

    /* 初始系统提示词（不含情绪：provider 尚未注册，rebuild 自然跳过） */
    rebuild_system_prompt();

    s_ctx.turn_count = 0;

    unlock();

    ESP_LOGI(TAG, "Ready: model=%s base=%s tokens=%d temp=%.1f",
             s_ctx.model, s_ctx.base_url, s_ctx.max_tokens, (double)s_ctx.temperature);
    return ESP_OK;
}

void llm_deinit(void)
{
    lock();
    if (s_ctx.chat) { s_ctx.oai->chatDelete(s_ctx.chat); s_ctx.chat = NULL; }
    if (s_ctx.oai)  { OpenAIDelete(s_ctx.oai);           s_ctx.oai  = NULL; }
    free(s_ctx.custom_prompt);
    s_ctx.custom_prompt = NULL;
    s_ctx.emotion_provider = NULL;
    s_ctx.turn_count = 0;
    unlock();

    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }
    ESP_LOGI(TAG, "Deinitialized");
}

/* ---- 情绪上下文（Pull 模式） ---- */

void llm_set_emotion_provider(llm_emotion_provider_t provider)
{
    lock();
    s_ctx.emotion_provider = provider;
    unlock();
    ESP_LOGI(TAG, "Emotion provider %s",
             provider ? "registered" : "unregistered");
}

void llm_set_system_prompt(const char *prompt)
{
    lock();
    free(s_ctx.custom_prompt);
    s_ctx.custom_prompt = NULL;

    if (prompt && prompt[0]) {
        s_ctx.custom_prompt = strdup(prompt);
    }
    rebuild_system_prompt();
    unlock();
}

/* ---- 同步聊天 ---- */

esp_err_t llm_chat(const char *user_message,
                   char *response_buf, size_t buf_size)
{
    return llm_chat_ex(user_message, response_buf, buf_size, NULL);
}

esp_err_t llm_chat_ex(const char *user_message,
                      char *response_buf, size_t buf_size,
                      int *out_http_code)
{
    if (!user_message || !response_buf || buf_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ctx.oai || !s_ctx.chat) {
        snprintf(response_buf, buf_size, "[LLM 未初始化]");
        return ESP_ERR_INVALID_STATE;
    }

    lock();

    /* Pull 模式：每次聊天前实时拉取最新情绪 → 构建系统提示词 */
    rebuild_system_prompt();

    int64_t t0 = esp_timer_get_time();

    /* 调用 espressif/openai: 发送消息并保存到对话历史 */
    OpenAI_StringResponse_t *resp =
        s_ctx.chat->multiModalMessage(s_ctx.chat, "text", user_message, true);

    int64_t dt = esp_timer_get_time() - t0;

    esp_err_t ret;
    if (resp) {
        ret = extract_response(resp, response_buf, buf_size);
        resp->deleteResponse(resp);
        if (ret == ESP_OK) s_ctx.turn_count++;
    } else {
        snprintf(response_buf, buf_size, "[网络错误：请求失败]");
        ESP_LOGE(TAG, "multiModalMessage returned NULL");
        ret = ESP_FAIL;
    }

    if (out_http_code) *out_http_code = (ret == ESP_OK) ? 200 : 0;

    ESP_LOGI(TAG, "Chat done: %lld ms, turn=%d", dt, s_ctx.turn_count);

    unlock();
    return ret;
}

/* ---- 异步聊天 ---- */

static void async_task(void *arg)
{
    async_ctx_t *ctx = (async_ctx_t *)arg;

    char resp[MAX_RESPONSE];
    int http_code = 0;
    esp_err_t ret = llm_chat_ex(ctx->message, resp, sizeof(resp), &http_code);

    if (ctx->cb) {
        ctx->cb((ret == ESP_OK) ? resp : NULL,
                (ret == ESP_OK), http_code, ctx->user_data);
    }

    free(ctx->message);
    free(ctx);

    lock();
    s_ctx.busy = false;
    unlock();

    vTaskDelete(NULL);
}

esp_err_t llm_chat_async(const char *user_message,
                         llm_chat_callback_t callback,
                         void *user_data)
{
    if (!user_message || !callback) return ESP_ERR_INVALID_ARG;

    lock();
    if (s_ctx.busy) {
        unlock();
        ESP_LOGW(TAG, "Already busy");
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.busy = true;
    unlock();

    async_ctx_t *ctx = (async_ctx_t *)calloc(1, sizeof(async_ctx_t));
    if (!ctx) { lock(); s_ctx.busy = false; unlock(); return ESP_ERR_NO_MEM; }

    ctx->message   = strdup(user_message);
    ctx->cb        = callback;
    ctx->user_data = user_data;

    if (!ctx->message) {
        free(ctx);
        lock(); s_ctx.busy = false; unlock();
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(async_task, "llm_async", ASYNC_STACK,
                    ctx, ASYNC_PRIO, NULL) != pdPASS) {
        free(ctx->message);
        free(ctx);
        lock(); s_ctx.busy = false; unlock();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ---- 对话历史 ---- */

void llm_clear_history(void)
{
    lock();
    if (s_ctx.chat) {
        s_ctx.chat->clearConversation(s_ctx.chat);
        rebuild_system_prompt();   /* 重新注入系统消息 */
        s_ctx.turn_count = 0;
        ESP_LOGI(TAG, "History cleared");
    }
    unlock();
}

int llm_get_turn_count(void)
{
    lock();
    int n = s_ctx.turn_count;
    unlock();
    return n;
}

/* ---- 状态 ---- */

bool llm_is_busy(void)
{
    lock();
    bool b = s_ctx.busy;
    unlock();
    return b;
}
