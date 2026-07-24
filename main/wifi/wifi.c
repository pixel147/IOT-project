/* ============================================================
 * wifi.c — WiFi Station 实现
 *
 * NVS 持久化凭据 + 非阻塞连接 + 状态回调。
 * ============================================================ */

#include "wifi.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

static const char *TAG = "WIFI";

#define NVS_NAMESPACE  "wifi_creds"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "password"

/* ---- 内部状态 ---- */
static esp_netif_t           *s_netif    = NULL;
static EventGroupHandle_t     s_evt      = NULL;
static SemaphoreHandle_t      s_mutex    = NULL;
static volatile wifi_status_t s_status   = WIFI_STATUS_DISCONNECTED;
static volatile bool          s_inited   = false;
static wifi_status_cb_t       s_cb       = NULL;
static char                   s_pending_ssid[34];  /* 33 + 1 extra to suppress trunc warning */
static char                   s_pending_pass[66]; /* 65 + 1 extra */
static volatile bool          s_auto_connect = false;  /* true → WIFI_EVENT_STA_START 后自动 connect */

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ---- 内部：更新状态并通知回调 ---- */
static void set_status(wifi_status_t st)
{
    wifi_status_t old;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    old = s_status;
    s_status = st;
    if (s_mutex) xSemaphoreGive(s_mutex);

    if (st != old && s_cb) {
        s_cb(st);
    }
}

/* ---- 内部：从 NVS 读取凭据 ---- */
static bool load_creds(char *ssid, size_t ssid_sz,
                       char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = ssid_sz;
    esp_err_t r = nvs_get_str(h, NVS_KEY_SSID, ssid, &len);
    if (r != ESP_OK) { nvs_close(h); return false; }

    len = pass_sz;
    r = nvs_get_str(h, NVS_KEY_PASS, pass, &len);
    if (r != ESP_OK) pass[0] = '\0';  /* 开放网络 */

    nvs_close(h);
    return true;
}

/* ---- 事件回调 ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_auto_connect) {
                ESP_LOGI(TAG, "STA started — connecting…");
                set_status(WIFI_STATUS_CONNECTING);
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "STA started — idle (scan-only mode)");
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *d =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected (reason=%d), reconnecting…", d->reason);
            set_status(WIFI_STATUS_DISCONNECTED);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        set_status(WIFI_STATUS_CONNECTED);
        if (s_evt) xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    }
}

/* ---- 内部：应用 WiFi 配置并启动 ---- */
static esp_err_t apply_and_start(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    /* Use bounded memcpy: ESP-IDF's sta.ssid/password are fixed max 32/64 byte arrays.
     * strncpy triggers -Wstringop-truncation when src length equals dst size. */
    size_t n = strlen(ssid);
    if (n >= sizeof(cfg.sta.ssid)) n = sizeof(cfg.sta.ssid) - 1;
    memcpy(cfg.sta.ssid, ssid, n);
    cfg.sta.ssid[n] = '\0';

    if (password && password[0]) {
        n = strlen(password);
        if (n >= sizeof(cfg.sta.password)) n = sizeof(cfg.sta.password) - 1;
        memcpy(cfg.sta.password, password, n);
        cfg.sta.password[n] = '\0';
    }
    cfg.sta.threshold.authmode = (password && password[0])
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;

    /* 先停再配再启，确保换网时干净切换 */
    esp_wifi_stop();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "Connecting to %s…", ssid);
    return ESP_OK;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

esp_err_t wifi_init(void)
{
    if (s_inited) return ESP_OK;

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 网络栈 */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    /* 事件 */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
        TAG, "wifi event register");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
        TAG, "ip event register");

    /* 同步对象 */
    s_evt   = xEventGroupCreate();
    s_mutex = xSemaphoreCreateMutex();
    if (!s_evt || !s_mutex) return ESP_ERR_NO_MEM;

    /* 启动 STA 模式（不连接，仅开启无线电以支持扫描） */
    s_auto_connect = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    s_inited = true;
    ESP_LOGI(TAG, "Stack initialized (STA idle, scan-ready)");
    return ESP_OK;
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "Call wifi_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    /* NULL ssid → 从 NVS 加载 */
    char buf_ssid[33], buf_pass[65];
    if (!ssid) {
        if (!load_creds(buf_ssid, sizeof(buf_ssid),
                        buf_pass, sizeof(buf_pass))) {
            ESP_LOGI(TAG, "No saved credentials — skipping auto-connect");
            return ESP_OK;   /* 不是错误，只是尚无凭据 */
        }
        ssid     = buf_ssid;
        password = buf_pass;
        ESP_LOGI(TAG, "Auto-connecting with saved credentials: %s", ssid);
    }

    if (!password) password = "";

    /* 保存到 pending（供连接成功后自动 save） */
    size_t n = strlen(ssid);
    if (n >= sizeof(s_pending_ssid)) n = sizeof(s_pending_ssid) - 1;
    memcpy(s_pending_ssid, ssid, n);
    s_pending_ssid[n] = '\0';

    n = strlen(password);
    if (n >= sizeof(s_pending_pass)) n = sizeof(s_pending_pass) - 1;
    memcpy(s_pending_pass, password, n);
    s_pending_pass[n] = '\0';

    /* 清除之前的失败位 + 启用自动连接 */
    s_auto_connect = true;
    if (s_evt) xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    return apply_and_start(ssid, password);
}

void wifi_disconnect(void)
{
    if (!s_inited) return;
    ESP_LOGI(TAG, "Disconnecting…");
    esp_wifi_disconnect();
    esp_wifi_stop();
    set_status(WIFI_STATUS_DISCONNECTED);
}

/* ---- 状态查询 ---- */

wifi_status_t wifi_get_status(void)
{
    wifi_status_t st;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    st = s_status;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return st;
}

bool wifi_is_connected(void)
{
    return wifi_get_status() == WIFI_STATUS_CONNECTED;
}

esp_err_t wifi_get_ip(char *buf, size_t size)
{
    if (!s_netif || !buf || size < 8) return ESP_ERR_INVALID_ARG;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) != ESP_OK)
        return ESP_ERR_INVALID_STATE;

    snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

void wifi_set_status_callback(wifi_status_cb_t cb)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cb = cb;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

/* ---- NVS 凭据 ---- */

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!password) password = "";

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h),
                        TAG, "nvs_open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_SSID, ssid), TAG, "set ssid");
    esp_err_t r = nvs_set_str(h, NVS_KEY_PASS, password);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);

    if (r == ESP_OK)
        ESP_LOGI(TAG, "Credentials saved: %s", ssid);
    return r;
}

bool wifi_has_saved_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 1;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, NULL, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

esp_err_t wifi_get_saved_ssid(char *buf, size_t size)
{
    if (!buf || size == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h),
                        TAG, "nvs_open");
    esp_err_t r = nvs_get_str(h, NVS_KEY_SSID, buf, &size);
    nvs_close(h);
    return r;
}

esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h),
                        TAG, "nvs_open");
    ESP_RETURN_ON_ERROR(nvs_erase_key(h, NVS_KEY_SSID), TAG, "erase ssid");
    nvs_erase_key(h, NVS_KEY_PASS);  /* 忽略：可能本就不存在 */
    esp_err_t r = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials cleared");
    return r;
}

/* ---- 网络连通性测试（DNS 解析 bilibili.com） ---- */

esp_err_t wifi_ping_test(int *latency_ms)
{
    if (wifi_is_connected() == false) return ESP_ERR_INVALID_STATE;

    int64_t t0 = esp_timer_get_time();
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo("bilibili.com", NULL, &hints, &res);
    int64_t dt = esp_timer_get_time() - t0;

    if (latency_ms) *latency_ms = (int)(dt / 1000);

    if (rc == 0 && res) {
        char ip[16];
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(addr->sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "Ping bilibili.com → %s (%lld ms)", ip, dt / 1000);
        freeaddrinfo(res);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Ping bilibili.com failed: rc=%d", rc);
    if (res) freeaddrinfo(res);
    return ESP_FAIL;
}
