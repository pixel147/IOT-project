/* ============================================================
 * wifi.h — WiFi Station 连接管理
 *
 * 用于情绪守护项目的大模型联网需求。
 * 提供简洁的连接 / 状态 / 断开 API。
 *
 * 接入步骤（在 app_main.c 中）：
 *   1. wifi_connect(SSID, PASSWORD)
 *   2. wifi_wait_connected(15000)   // 最多等 15 秒
 *   3. 之后即可使用 llm_client
 *
 * 依赖: nvs_flash, esp_netif, esp_wifi, esp_event
 * ============================================================ */

#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 连接 WiFi Station（非阻塞，立即返回）
 *
 * 内部完成 netif / wifi 初始化和事件循环创建，
 * 可安全重复调用（幂等 — 已连接则直接返回 ESP_OK）。
 *
 * @param ssid     WiFi SSID
 * @param password WiFi 密码（开放网络传 "" 或 NULL）
 * @return ESP_OK / ESP_FAIL / ESP_ERR_NO_MEM
 */
esp_err_t wifi_connect(const char *ssid, const char *password);

/**
 * @brief 阻塞等待 WiFi 连接成功
 * @param timeout_ms 超时毫秒数，0 表示无限等待
 * @return ESP_OK 已连接 / ESP_ERR_TIMEOUT 超时
 */
esp_err_t wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief 查询当前 WiFi 是否已连接（拿到 IP）
 */
bool wifi_is_connected(void);

/**
 * @brief 断开 WiFi 并释放资源
 */
void wifi_disconnect(void);

/**
 * @brief 获取当前 IP 地址字符串（如 "192.168.1.100"）
 * @param buf  输出缓冲区
 * @param size 缓冲区大小（建议 >= 16）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（未连接）
 */
esp_err_t wifi_get_ip(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */
