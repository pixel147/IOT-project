/* ============================================================
 * llm_config.h — 大模型接入配置
 *
 * 适配任意兼容 OpenAI Chat Completions API 的服务商，
 * 底层使用 espressif/openai 组件 (esp_crt_bundle TLS 验证)。
 *
 * 预设 Base URL（尾部带 /，组件自动追加 chat/completions）：
 *   - OpenAI:     https://api.openai.com/v1/
 *   - DeepSeek:   https://api.deepseek.com/v1/
 *   - 通义千问:    https://dashscope.aliyuncs.com/compatible-mode/v1/
 *   - Groq:       https://api.groq.com/openai/v1/
 *   - Together:   https://api.together.xyz/v1/
 *   - 本地 Ollama: http://192.168.x.x:11434/v1/
 *
 * ⚠️ 敏感凭据：不要提交 api_key 到版本控制！
 * ============================================================ */

#ifndef LLM_CONFIG_H
#define LLM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 预设 Base URL（组件自动追加 chat/completions）
 * ============================================================ */

#define LLM_BASE_OPENAI       "https://api.openai.com/v1/"
#define LLM_BASE_DEEPSEEK     "https://api.deepseek.com/v1/"
#define LLM_BASE_QWEN         "https://dashscope.aliyuncs.com/compatible-mode/v1/"
#define LLM_BASE_GROQ         "https://api.groq.com/openai/v1/"
#define LLM_BASE_TOGETHER     "https://api.together.xyz/v1/"
#define LLM_BASE_OLLAMA       "http://192.168.1.100:11434/v1/"

/* ============================================================
 * 预设模型名
 * ============================================================ */

#define LLM_MODEL_DEEPSEEK_V4_FLASH "deepseek-v4-flash"

/* ============================================================
 * 默认参数
 * ============================================================ */

#define LLM_DEFAULT_MAX_TOKENS    256
#define LLM_DEFAULT_TEMPERATURE   0.8f
#define LLM_DEFAULT_TIMEOUT_MS    30000

/* ============================================================
 * 情绪提示词（7 条，对应 emotion class 0-6）
 * ============================================================ */

extern const char *LLM_EMOTION_NAMES[];    /* {"生气","厌恶","害怕","开心","难过","惊讶","平静"} */
extern const char *LLM_EMOTION_PROMPTS[];  /* 每条情绪对应的系统提示词 */

/* ============================================================
 * 配置结构体
 *
 * 最小示例：
 *   llm_config_t cfg = {
 *       .base_url = LLM_BASE_DEEPSEEK,
 *       .api_key  = "sk-xxxxxxxx",
 *       .model    = LLM_MODEL_DEEPSEEK_V4_FLASH,
 *   };
 *   llm_init(&cfg);
 * ============================================================ */

typedef struct {
    /** Base URL（尾部带 /），组件自动追加 chat/completions */
    const char *base_url;

    /** API Key / Bearer Token */
    const char *api_key;

    /** 模型名称 */
    const char *model;

    /** 最大生成 token 数（0 = 默认 256） */
    int         max_tokens;

    /** 采样温度 (0-2)，默认 0.8 */
    float       temperature;

    /** HTTP 超时毫秒（0 = 30000，组件内部处理） */
    int         timeout_ms;
} llm_config_t;

#ifdef __cplusplus
}
#endif

#endif /* LLM_CONFIG_H */
