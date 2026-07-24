/* ============================================================
 * wifi.h — WiFi Station 连接管理
 *
 * 用于情绪守护项目的大模型联网需求。
 * 凭据通过 NVS 持久化，支持运行时配置。
 *
 * 典型启动流程：
 *   1. wifi_init()                     // 初始化栈，不连接
 *   2. wifi_set_status_callback(cb)    // 注册状态回调
 *   3. wifi_connect(NULL)              // 尝试 NVS 凭据自动连接
 *
 * 运行时配置（设置页）：
 *   1. wifi_connect("MyWiFi", "pwd")   // 连接新网络
 *   2. wifi_save_credentials(ssid, pw) // 持久化到 NVS
 *
 * 依赖: nvs_flash, esp_netif, esp_wifi, esp_event
 * ============================================================ */

#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 状态
 * ============================================================ */

typedef enum {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_ERROR,
} wifi_status_t;

/** WiFi 状态变化回调（在 event handler 上下文中调用，不阻塞） */
typedef void (*wifi_status_cb_t)(wifi_status_t status);

/* ============================================================
 * 生命周期
 * ============================================================ */

/**
 * @brief 初始化 WiFi 栈（nvs/netif/event/wifi_init）
 *
 * 幂等 — 可安全重复调用。不会自动连接任何 AP。
 *
 * @return ESP_OK / ESP_FAIL
 */
esp_err_t wifi_init(void);

/**
 * @brief 连接 WiFi Station（非阻塞，立即返回）
 *
 * @param ssid     WiFi SSID — 传 NULL 表示从 NVS 读取已保存凭据
 * @param password WiFi 密码 — ssid 非 NULL 时必需；开放网络传 ""
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE
 */
esp_err_t wifi_connect(const char *ssid, const char *password);

/**
 * @brief 断开当前 WiFi 连接
 */
void wifi_disconnect(void);

/* ============================================================
 * 状态查询
 * ============================================================ */

wifi_status_t wifi_get_status(void);
bool          wifi_is_connected(void);
esp_err_t     wifi_get_ip(char *buf, size_t size);

/** 注册状态回调（传 NULL 取消） */
void wifi_set_status_callback(wifi_status_cb_t cb);

/* ============================================================
 * NVS 凭据管理
 * ============================================================ */

/**
 * @brief 保存凭据到 NVS（下次启动可用 wifi_connect(NULL) 自动连接）
 */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/** 查询 NVS 中是否有已保存的凭据 */
bool wifi_has_saved_credentials(void);

/** 读取已保存的 SSID（buf 建议 >= 33 字节） */
esp_err_t wifi_get_saved_ssid(char *buf, size_t size);

/** 清除 NVS 中保存的凭据 */
esp_err_t wifi_clear_credentials(void);

/* ============================================================
 * 网络连通性测试
 * ============================================================ */

/**
 * @brief DNS 解析 bilibili.com，测试网络是否真正连通
 * @param latency_ms 输出解析耗时（毫秒），可传 NULL
 * @return ESP_OK 连通 / ESP_FAIL 不通
 */
esp_err_t wifi_ping_test(int *latency_ms);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */
