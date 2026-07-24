/* ============================================================
 * wifi.c — WiFi Station 实现
 *
 * 基于 ESP-IDF wifi station 例程，封装为简洁 API。
 * 使用 FreeRTOS 事件组同步连接状态。
 * ============================================================ */

#include "wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI";

/* ---- 内部状态 ---- */
static esp_netif_t           *s_netif = NULL;
static EventGroupHandle_t     s_evt   = NULL;
static volatile bool          s_connected = false;
static volatile bool          s_inited    = false;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ---- 事件回调 ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)arg; (void)event_data;

    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *d =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected (reason=%d), reconnecting…", d->reason);
            s_connected = false;
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        if (s_evt) xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    }
}

/* ---- 公共 API ---- */

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    if (s_connected) {
        ESP_LOGI(TAG, "Already connected");
        return ESP_OK;
    }

    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID required");
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 一次性初始化（幂等） ---- */
    if (!s_inited) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_netif = esp_netif_create_default_wifi_sta();

        /* P4 无原生 WiFi，通过 SDIO 与 ESP32-C6 通信 */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_remote_init(&cfg);
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        /* 事件组 */
        s_evt = xEventGroupCreate();
        if (!s_evt) return ESP_ERR_NO_MEM;

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            wifi_event_handler, NULL, NULL));

        s_inited = true;
    } else {
        /* 非首次调用：重置事件组比特位 */
        xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    /* ---- 配置 WiFi ---- */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
    }
    wifi_cfg.sta.threshold.authmode = password && password[0]
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s…", ssid);
    return ESP_OK;
}

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_connected) return ESP_OK;
    if (!s_evt)   return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == 0)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(
        s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, ticks);

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_get_ip(char *buf, size_t size)
{
    if (!s_connected || !s_netif || !buf || size < 8) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_netif, &ip_info) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(buf, size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

void wifi_disconnect(void)
{
    if (!s_inited) return;
    ESP_LOGI(TAG, "Disconnecting…");
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_connected = false;
}
