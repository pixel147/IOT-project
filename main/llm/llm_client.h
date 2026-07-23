/* ============================================================
 * llm_client.h — 大模型客户端（espressif/openai 封装）
 *
 * 底层使用 espressif/openai 官方组件：
 *   - esp_crt_bundle TLS 证书验证
 *   - 对话历史自动管理
 *   - 多模态支持（预留）
 *
 * 本层提供：
 *   - 情绪感知系统提示词注入
 *   - 简化的同步 / 异步 C API
 *   - 线程安全
 *
 * 接入步骤（在 app_main.c 中）：
 *   1. WiFi 已连接
 *   2. llm_config_t cfg = { .base_url=LLM_BASE_DEEPSEEK, .api_key=…, .model=… };
 *   3. llm_init(&cfg)
 *   4. llm_set_emotion_provider(provider_cb)  // 注册情绪提供者回调
 *   5. llm_chat("你好", buf, sizeof(buf))      // 每次自动带最新情绪
 *
 * 依赖: espressif/openai, FreeRTOS
 * ============================================================ */

#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include "llm_config.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 异步回调
 * ============================================================ */

typedef void (*llm_chat_callback_t)(const char *response, bool success,
                                    int http_status, void *user_data);

/* ============================================================
 * 生命周期
 * ============================================================ */

esp_err_t llm_init(const llm_config_t *config);
void      llm_deinit(void);

/* ============================================================
 * 情绪上下文 — Pull 模式（核心增值功能）
 *
 * 不再由 emotion_task 主动 push，改为在每次 chat 前
 * 通过回调拉取最新情绪，保证单一数据源、零延迟。
 * ============================================================ */

/** 情绪提供者回调：LLM 在每次 chat 前调用，获取最新情绪 */
typedef void (*llm_emotion_provider_t)(int *emotion_class, float *confidence);

/** 注册情绪提供者（传 NULL 取消情绪感知） */
void llm_set_emotion_provider(llm_emotion_provider_t provider);

/** 手动覆盖系统提示词（传 NULL 恢复默认情绪模板） */
void llm_set_system_prompt(const char *prompt);

/* ============================================================
 * 同步聊天
 * ============================================================ */

/** 发送消息，阻塞等待回复 */
esp_err_t llm_chat(const char *user_message,
                   char *response_buf, size_t buf_size);

/** 发送消息并获取 HTTP 状态码 */
esp_err_t llm_chat_ex(const char *user_message,
                      char *response_buf, size_t buf_size,
                      int *out_http_code);

/* ============================================================
 * 异步聊天
 * ============================================================ */

/** 异步聊天（立即返回，结果通过回调通知） */
esp_err_t llm_chat_async(const char *user_message,
                         llm_chat_callback_t callback,
                         void *user_data);

/* ============================================================
 * 对话历史
 * ============================================================ */

void llm_clear_history(void);
int  llm_get_turn_count(void);

/* ============================================================
 * 状态
 * ============================================================ */

bool llm_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLIENT_H */
