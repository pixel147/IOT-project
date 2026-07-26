#include "voice_xiaozhi.h"

#include "bsp/esp32_p4_function_ev_board.h"
#include "driver/i2s_std.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "mqtt_client.h"
#include "esp_wifi.h"

#include <mbedtls/aes.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cJSON.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

#include "chat.h"

namespace {

constexpr const char *TAG = "XIAOZHI_VOICE";
constexpr int kSampleRate = 16000;
constexpr int kOpusFrameSamples = 960;  // 60 ms at 16 kHz.
constexpr EventBits_t kServerHello = BIT0;
constexpr EventBits_t kMqttConnected = BIT1;
constexpr EventBits_t kMcpDone = BIT2;
constexpr int kMqttTimeoutMs = 15000;
constexpr int kAesKeyLen = 16;

/* AES key/nonce from server hello for UDP audio encryption */
struct UdpKey {
    uint8_t key[kAesKeyLen];
    uint8_t nonce[kAesKeyLen];
};

struct VoiceState {
    esp_codec_dev_handle_t codec = nullptr;
    const audio_codec_data_if_t *data_if = nullptr;
    const audio_codec_ctrl_if_t *ctrl_if = nullptr;
    const audio_codec_if_t *codec_if = nullptr;
    const audio_codec_gpio_if_t *gpio_if = nullptr;
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_handle_t rx = nullptr;
    srmodel_list_t *models = nullptr;
    const esp_afe_sr_iface_t *afe_iface = nullptr;
    esp_afe_sr_data_t *afe = nullptr;
    void *opus_enc = nullptr;
    int opus_enc_out_sz = 0;
    EventGroupHandle_t events = nullptr;
    char session_id[96] = {};
    char uuid[96] = {};
    char device_id[18] = {};
    std::atomic<bool> manual_start_requested{false};
    std::atomic<bool> session_active{false};
    std::atomic<bool> turn_finished{false};
    bool listening = false;
    int silence_frames = 0;
    std::vector<int16_t> opus_pcm;

    /* MQTT signaling */
    esp_mqtt_client_handle_t mqtt = nullptr;
    char mqtt_endpoint[128] = {};
    char mqtt_client_id[128] = {};
    char mqtt_username[128] = {};
    char mqtt_password[128] = {};
    char mqtt_publish_topic[64] = {};
    char mqtt_p2p_topic[64] = {};   /* devices/p2p/{mac} — server pushes messages here */

    /* UDP audio send */
    int udp_fd = -1;
    char udp_server[64] = {};
    int udp_port = 0;
    mbedtls_aes_context aes_ctx;
    UdpKey udp_key;
    uint32_t local_sequence = 0;

    /* Task-local buffer (was static) */
    int16_t *afe_buf = nullptr;
    int afe_buf_sz = 0;

    /* One-shot OTA */
    bool ota_registered = false;
};

VoiceState s_voice;

/* Get the C6 coprocessor's actual WiFi MAC (not the P4's base MAC).
 * Fall back to base MAC if wifi is not yet initialized. */
static void get_wifi_mac(uint8_t mac[6])
{
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK || (mac[0] == 0 && mac[1] == 0 && mac[2] == 0)) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
}

/* ---- MQTT + UDP protocol ---- */

/* Send a JSON string via MQTT publish (returns msg_id or -1). */
static int send_text(const char *json)
{
    if (s_voice.mqtt && s_voice.mqtt_publish_topic[0]) {
        return esp_mqtt_client_publish(s_voice.mqtt, s_voice.mqtt_publish_topic,
                                       json, 0, 1, 0);
    }
    return -1;
}

static void stop_listening(const char *status)
{
    if (s_voice.listening && s_voice.session_id[0]) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
                 s_voice.session_id);
        send_text(msg);
    }
    s_voice.listening = false;
    s_voice.silence_frames = 0;
    s_voice.opus_pcm.clear();
    if (status) ui_chat_voice_set_status(status);
}

static bool load_mqtt_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open("mqtt", NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t sz = sizeof(s_voice.mqtt_endpoint);
    esp_err_t err = nvs_get_str(nvs, "endpoint", s_voice.mqtt_endpoint, &sz);
    sz = sizeof(s_voice.mqtt_client_id);
    if (err == ESP_OK) err = nvs_get_str(nvs, "client_id", s_voice.mqtt_client_id, &sz);
    sz = sizeof(s_voice.mqtt_username);
    if (err == ESP_OK) err = nvs_get_str(nvs, "username", s_voice.mqtt_username, &sz);
    sz = sizeof(s_voice.mqtt_password);
    if (err == ESP_OK) err = nvs_get_str(nvs, "password", s_voice.mqtt_password, &sz);
    sz = sizeof(s_voice.mqtt_publish_topic);
    if (err == ESP_OK) err = nvs_get_str(nvs, "publish_topic", s_voice.mqtt_publish_topic, &sz);
    nvs_close(nvs);

    return err == ESP_OK && s_voice.mqtt_endpoint[0] && s_voice.mqtt_client_id[0] &&
           s_voice.mqtt_username[0] && s_voice.mqtt_password[0] && s_voice.mqtt_publish_topic[0];
}

/* ---- AES-128-CTR encrypt and send one Opus frame via UDP ---- */
/* Nonce: [2B const][2B payload_len][4B const][4B timestamp][4B seq] */

static void udp_send_opus(const int16_t *pcm)
{
    if (s_voice.udp_fd < 0 || !s_voice.opus_enc) return;

    /* Opus encode */
    std::vector<uint8_t> opus(s_voice.opus_enc_out_sz);
    esp_audio_enc_in_frame_t input = {
        .buffer = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(pcm)),
        .len = static_cast<uint32_t>(kOpusFrameSamples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t encoded = {
        .buffer = opus.data(),
        .len = static_cast<uint32_t>(opus.size()),
        .encoded_bytes = 0,
    };
    if (esp_opus_enc_process(s_voice.opus_enc, &input, &encoded) != ESP_AUDIO_ERR_OK ||
        encoded.encoded_bytes == 0) return;

    /* Build nonce */
    uint8_t nonce[kAesKeyLen];
    memcpy(nonce, s_voice.udp_key.nonce, 2);
    nonce[2] = (uint8_t)((encoded.encoded_bytes >> 8) & 0xff);
    nonce[3] = (uint8_t)(encoded.encoded_bytes & 0xff);
    memcpy(nonce + 4, s_voice.udp_key.nonce + 4, 4);
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
    nonce[8]  = (uint8_t)(ts >> 24); nonce[9]  = (uint8_t)(ts >> 16);
    nonce[10] = (uint8_t)(ts >> 8);  nonce[11] = (uint8_t)(ts);
    uint32_t seq = s_voice.local_sequence++;
    nonce[12] = (uint8_t)(seq >> 24); nonce[13] = (uint8_t)(seq >> 16);
    nonce[14] = (uint8_t)(seq >> 8);  nonce[15] = (uint8_t)(seq);

    /* AES-128-CTR encrypt */
    size_t elen = encoded.encoded_bytes;
    std::vector<uint8_t> cipher(elen);
    uint8_t stream_block[16] = {};
    size_t offset = 0;
    mbedtls_aes_crypt_ctr(&s_voice.aes_ctx, elen, &offset,
                          nonce, stream_block, opus.data(), cipher.data());

    /* UDP send: [16B nonce][ciphertext] */
    std::vector<uint8_t> packet;
    packet.reserve(16 + elen);
    packet.insert(packet.end(), nonce, nonce + 16);
    packet.insert(packet.end(), cipher.begin(), cipher.end());

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(s_voice.udp_port);
    inet_pton(AF_INET, s_voice.udp_server, &dest.sin_addr);
    sendto(s_voice.udp_fd, packet.data(), packet.size(), 0,
           (struct sockaddr *)&dest, sizeof(dest));
}

/* ---- MQTT event handler ---- */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *edata)
{
    (void)arg; (void)base;
    auto &e = *static_cast<esp_mqtt_event_t *>(edata);

    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected");
        /* Subscribe to P2P topic: devices/p2p/{mac_with_underscores} */
        {
            char p2p_topic[64];
            snprintf(p2p_topic, sizeof(p2p_topic), "devices/p2p/%s", s_voice.device_id);
            /* Replace colons with underscores */
            for (char *p = p2p_topic; *p; p++) if (*p == ':') *p = '_';
            esp_mqtt_client_subscribe(s_voice.mqtt, p2p_topic, 1);
            strlcpy(s_voice.mqtt_p2p_topic, p2p_topic, sizeof(s_voice.mqtt_p2p_topic));
            ESP_LOGI(TAG, "Subscribed to %s", p2p_topic);
        }
        xEventGroupSetBits(s_voice.events, kMqttConnected);

    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "MQTT disconnected");
        s_voice.listening = false;
        s_voice.session_id[0] = '\0';
        if (s_voice.session_active.load()) {
            ui_chat_voice_set_status("Voice: connection lost");
        }

    } else if (event_id == MQTT_EVENT_DATA) {
        ESP_LOGI(TAG, "MQTT data on topic=%.*s len=%d %.*s",
                 e.topic_len, e.topic, e.data_len,
                 e.data_len > 100 ? 100 : e.data_len, e.data);
        /* Save P2P topic from first incoming message */
        if (e.topic_len > 0 && !s_voice.mqtt_p2p_topic[0] && e.topic_len < 64) {
            strlcpy(s_voice.mqtt_p2p_topic, e.topic, e.topic_len + 1);
        }
        std::string payload(e.data, e.data_len);
        cJSON *root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (!root) return;

        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }
        const char *t = type->valuestring;

        if (strcmp(t, "hello") == 0) {
            cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            if (cJSON_IsString(session)) {
                strlcpy(s_voice.session_id, session->valuestring, sizeof(s_voice.session_id));
            }
            /* Parse UDP audio config from server hello */
            cJSON *udp = cJSON_GetObjectItemCaseSensitive(root, "udp");
            if (cJSON_IsObject(udp)) {
                cJSON *sv = cJSON_GetObjectItemCaseSensitive(udp, "server");
                cJSON *pt = cJSON_GetObjectItemCaseSensitive(udp, "port");
                cJSON *ky = cJSON_GetObjectItemCaseSensitive(udp, "key");
                cJSON *nc = cJSON_GetObjectItemCaseSensitive(udp, "nonce");
                if (cJSON_IsString(sv)) strlcpy(s_voice.udp_server, sv->valuestring, sizeof(s_voice.udp_server));
                if (cJSON_IsNumber(pt))  s_voice.udp_port = pt->valueint;
                if (cJSON_IsString(ky)) {
                    const char *hex = ky->valuestring;
                    for (int i = 0; i < kAesKeyLen && *hex && *(hex + 1); i++, hex += 2)
                        sscanf(hex, "%2hhx", &s_voice.udp_key.key[i]);
                }
                if (cJSON_IsString(nc)) {
                    const char *hex = nc->valuestring;
                    for (int i = 0; i < kAesKeyLen && *hex && *(hex + 1); i++, hex += 2)
                        sscanf(hex, "%2hhx", &s_voice.udp_key.nonce[i]);
                }
            }
            xEventGroupSetBits(s_voice.events, kServerHello);

        } else if (strcmp(t, "stt") == 0) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(text) && text->valuestring[0]) {
                ui_chat_voice_append_user(text->valuestring);
            }

        } else if (strcmp(t, "tts") == 0) {
            cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(state) && cJSON_IsString(text) && text->valuestring[0]) {
                if (strcmp(state->valuestring, "sentence_end") == 0 ||
                    strcmp(state->valuestring, "stop") == 0) {
                    ui_chat_voice_append_assistant(text->valuestring);
                }
            }
            if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0) {
                s_voice.turn_finished.store(true);
                stop_listening("Voice: ready");
            }

        } else if (strcmp(t, "mcp") == 0) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "payload");
            cJSON *method = p ? cJSON_GetObjectItemCaseSensitive(p, "method") : nullptr;
            cJSON *msg_id = p ? cJSON_GetObjectItemCaseSensitive(p, "id") : nullptr;
            cJSON *msg_ss = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            const char *resp_ss = cJSON_IsString(msg_ss) ? msg_ss->valuestring : s_voice.session_id;
            char sess_f[128] = {};
            if (resp_ss && resp_ss[0]) snprintf(sess_f, sizeof(sess_f), ",\"session_id\":\"%s\"", resp_ss);
            char mcp_resp[512] = {};
            if (cJSON_IsString(method)) {
                int id = cJSON_IsNumber(msg_id) ? msg_id->valueint : 1;
                if (strcmp(method->valuestring, "initialize") == 0) {
                    snprintf(mcp_resp, sizeof(mcp_resp),
                             "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
                             "\"result\":{\"protocolVersion\":\"2024-11-05\","
                             "\"capabilities\":{\"audio\":{\"input\":true,\"output\":false},\"vad\":true},"
                             "\"serverInfo\":{\"name\":\"esp32-p4\",\"version\":\"2.4.0\"}}}%s}",
                             id, sess_f);
                } else if (strcmp(method->valuestring, "tools/list") == 0) {
                    snprintf(mcp_resp, sizeof(mcp_resp),
                             "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
                             "\"result\":{\"tools\":[]}}%s}", id, sess_f);
                }
                if (mcp_resp[0]) {
                    int msg_id = -1;
                    if (s_voice.mqtt_p2p_topic[0]) {
                        msg_id = esp_mqtt_client_publish(s_voice.mqtt,
                                    s_voice.mqtt_p2p_topic, mcp_resp, 0, 1, 0);
                    }
                    ESP_LOGI(TAG, "MCP %s respond msg_id=%d p2p=%s",
                             method->valuestring, msg_id, s_voice.mqtt_p2p_topic);
                    xEventGroupSetBits(s_voice.events, kMcpDone);
                }
            }
        }
        cJSON_Delete(root);
    } else if (event_id == MQTT_EVENT_SUBSCRIBED) {
        ESP_LOGI(TAG, "MQTT subscribe OK (msg_id=%d)", e.msg_id);
    }
}

/* ---- Open audio channel: MQTT connect + subscribe + hello + UDP socket ---- */

static bool open_audio_channel(void)
{
    /* Clean up previous session */
    if (s_voice.mqtt) {
        esp_mqtt_client_stop(s_voice.mqtt);
        esp_mqtt_client_destroy(s_voice.mqtt);
        s_voice.mqtt = nullptr;
    }
    if (s_voice.udp_fd >= 0) {
        close(s_voice.udp_fd);
        s_voice.udp_fd = -1;
    }
    xEventGroupClearBits(s_voice.events, kServerHello | kMqttConnected | kMcpDone);
    s_voice.session_id[0] = '\0';
    memset(&s_voice.udp_key, 0, sizeof(s_voice.udp_key));
    s_voice.local_sequence = 0;

    ESP_LOGI(TAG, "MQTT %s:8883 as %s", s_voice.mqtt_endpoint, s_voice.mqtt_client_id);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname = s_voice.mqtt_endpoint;
    cfg.broker.address.port = 8883;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    cfg.credentials.client_id = s_voice.mqtt_client_id;
    cfg.credentials.username = s_voice.mqtt_username;
    cfg.credentials.authentication.password = s_voice.mqtt_password;
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.session.disable_clean_session = true;
    cfg.network.timeout_ms = kMqttTimeoutMs;

    s_voice.mqtt = esp_mqtt_client_init(&cfg);
    if (!s_voice.mqtt) return false;

    esp_mqtt_client_register_event(s_voice.mqtt, MQTT_EVENT_ANY,
                                    mqtt_event_handler, nullptr);
    if (esp_mqtt_client_start(s_voice.mqtt) != ESP_OK) return false;

    EventBits_t bits = xEventGroupWaitBits(s_voice.events, kMqttConnected,
                                            pdTRUE, pdFALSE, pdMS_TO_TICKS(kMqttTimeoutMs));
    if (!(bits & kMqttConnected)) {
        ESP_LOGE(TAG, "MQTT connect timeout");
        return false;
    }

    /* Wait for MCP handshake to complete before sending hello */
    bits = xEventGroupWaitBits(s_voice.events, kMcpDone,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(bits & kMcpDone)) {
        ESP_LOGE(TAG, "MCP handshake timeout — proceeding anyway");
    }

    /* Send hello to signaling topic */
    char hello[384];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\","
             "\"features\":{\"mcp\":true,\"aec\":false,\"vad\":true},"
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
             "\"channels\":1,\"frame_duration\":60}}");
    int hello_id = send_text(hello);
    ESP_LOGI(TAG, "Hello sent msg_id=%d topic=%s", hello_id, s_voice.mqtt_publish_topic);

    /* Wait for server hello with UDP config */
    bits = xEventGroupWaitBits(s_voice.events, kServerHello,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & kServerHello)) {
        ESP_LOGE(TAG, "Server hello timeout");
        return false;
    }
    ESP_LOGI(TAG, "Session: %s", s_voice.session_id);

    /* Open UDP socket for audio send */
    if (s_voice.udp_server[0] && s_voice.udp_port > 0) {
        mbedtls_aes_init(&s_voice.aes_ctx);
        mbedtls_aes_setkey_enc(&s_voice.aes_ctx, s_voice.udp_key.key, 128);

        s_voice.udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s_voice.udp_fd >= 0) {
            struct sockaddr_in local = {};
            local.sin_family = AF_INET;
            local.sin_port = 0;
            bind(s_voice.udp_fd, (struct sockaddr *)&local, sizeof(local));
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
            setsockopt(s_voice.udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ESP_LOGI(TAG, "UDP -> %s:%d", s_voice.udp_server, s_voice.udp_port);
        }
    }
    return true;
}

/* ---- Close audio channel ---- */

static void close_audio_channel(void)
{
    if (s_voice.session_id[0]) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"goodbye\",\"session_id\":\"%s\"}", s_voice.session_id);
        send_text(msg);
    }
    s_voice.listening = false;
    s_voice.session_id[0] = '\0';
    if (s_voice.udp_fd >= 0) {
        close(s_voice.udp_fd);
        s_voice.udp_fd = -1;
        mbedtls_aes_free(&s_voice.aes_ctx);
    }
    if (s_voice.mqtt) {
        esp_mqtt_client_stop(s_voice.mqtt);
        esp_mqtt_client_destroy(s_voice.mqtt);
        s_voice.mqtt = nullptr;
    }
}

/* ---- Voice task: manual button → MQTT connect → listen → send Opus via UDP ---- */

static void voice_task(void *arg)
{
    (void)arg;

    while (1) {
        ui_chat_voice_set_status("Voice: ready");
        while (!s_voice.manual_start_requested.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ui_chat_voice_set_status("Voice: connecting...");
        s_voice.session_active.store(true);

        /* OTA once */
        if (!s_voice.ota_registered) {
            s_voice.ota_registered = (xiaozhi_ota_register() == ESP_OK);
        }
        if (!load_mqtt_credentials() || !open_audio_channel()) {
            ESP_LOGW(TAG, "Voice start failed");
            s_voice.session_active.store(false);
            close_audio_channel();
            ui_chat_voice_set_status("Voice: tap music to retry");
            continue;
        }

        char listen[256];
        snprintf(listen, sizeof(listen),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}",
                 s_voice.session_id);
        send_text(listen);
        s_voice.listening = true;
        s_voice.silence_frames = 0;
        s_voice.opus_pcm.clear();
        s_voice.turn_finished.store(false);
        ui_chat_voice_set_status("Voice: listening...");

        while (s_voice.mqtt && s_voice.session_id[0] && !s_voice.turn_finished.load()) {

            /* Read mic → AFE */
            if (!s_voice.afe_iface || !s_voice.afe || !s_voice.codec) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            int feed = s_voice.afe_iface->get_feed_chunksize(s_voice.afe);
            int need = feed * (int)sizeof(int16_t);
            if (!s_voice.afe_buf || s_voice.afe_buf_sz < need) {
                if (s_voice.afe_buf) heap_caps_free(s_voice.afe_buf);
                s_voice.afe_buf = (int16_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                s_voice.afe_buf_sz = s_voice.afe_buf ? need : 0;
                if (!s_voice.afe_buf) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            }
            size_t i2s_bytes = 0;
            if (i2s_channel_read(s_voice.rx, s_voice.afe_buf, need,
                                 &i2s_bytes, pdMS_TO_TICKS(50)) == ESP_OK && i2s_bytes > 0) {
                s_voice.afe_iface->feed(s_voice.afe, s_voice.afe_buf);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5)); continue;
            }

            afe_fetch_result_t *result = s_voice.afe_iface->fetch(s_voice.afe);
            if (!result || !s_voice.listening || result->data_size == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            /* Opus → AES → UDP send */
            const size_t samples = result->data_size / sizeof(int16_t);
            s_voice.opus_pcm.insert(s_voice.opus_pcm.end(),
                                     result->data, result->data + samples);
            while (s_voice.opus_pcm.size() >= kOpusFrameSamples) {
                udp_send_opus(s_voice.opus_pcm.data());
                s_voice.opus_pcm.erase(s_voice.opus_pcm.begin(),
                                       s_voice.opus_pcm.begin() + kOpusFrameSamples);
            }
            s_voice.silence_frames = (result->vad_state == VAD_SILENCE)
                                     ? s_voice.silence_frames + 1 : 0;
            if (s_voice.silence_frames > 30) {
                stop_listening("Voice: processing");
            }

            /* Server responses arrive via MQTT → mqtt_event_handler */
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        s_voice.session_active.store(false);
        close_audio_channel();
        ui_chat_voice_set_status("Voice: ready");
    }
}


/* ---- hardware init task (deferred to background) ---- */
static void voice_hardware_init_task(void *arg)
{
    (void)arg;

    /* ---- Mount NVS and initialise the stable board identity ---- */
    esp_err_t nvs_ret = nvs_flash_init();
    i2c_master_bus_handle_t i2c_handle = NULL;
    (void)i2c_handle;
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ui_chat_voice_set_status("Voice: NVS init failed");
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nvs_ret));
        goto fail;
    }

    /* UUID was already loaded and saved in voice_xiaozhi_start() */

    /* ---- Device ID (C6 Wi-Fi STA MAC) ---- */
    ESP_LOGI(TAG, "D_INIT: step1 device_id");
    {
        uint8_t mac[6];
        get_wifi_mac(mac);
        snprintf(s_voice.device_id, sizeof(s_voice.device_id),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    /* ---- I2C (use BSP-initialised bus) ---- */
    ESP_LOGI(TAG, "D_INIT: step2 I2C");
    ui_chat_voice_set_status("Voice: init I2C");
    bsp_i2c_init();
    i2c_handle = bsp_i2c_get_handle();

    /* ---- I2S (PCM audio to/from ES8311) ---- */
    ESP_LOGI(TAG, "D_INIT: step3 I2S");
    ui_chat_voice_set_status("Voice: init I2S");
    {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        cc.dma_desc_num = 6;
        cc.dma_frame_num = 240;
        i2s_new_channel(&cc, &s_voice.tx, &s_voice.rx);
        i2s_std_config_t i2s_cfg = {};
        i2s_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
        i2s_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        i2s_cfg.gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN,
        };
        i2s_channel_init_std_mode(s_voice.tx, &i2s_cfg);
        i2s_channel_init_std_mode(s_voice.rx, &i2s_cfg);
        i2s_channel_enable(s_voice.tx);
        ESP_LOGI(TAG, "D_INIT: step3b I2S RX enabled");
        i2s_channel_enable(s_voice.rx);
    }

    /* ---- ES8311 audio codec ---- */
    ESP_LOGI(TAG, "D_INIT: step4 codec");
    ui_chat_voice_set_status("Voice: init codec");
    {
        audio_codec_i2s_cfg_t dc = {.port = I2S_NUM_0, .rx_handle = s_voice.rx, .tx_handle = s_voice.tx};
        s_voice.data_if = audio_codec_new_i2s_data(&dc);
        audio_codec_i2c_cfg_t cc = {.port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle};
        s_voice.ctrl_if = audio_codec_new_i2c_ctrl(&cc);
        s_voice.gpio_if = audio_codec_new_gpio();
        es8311_codec_cfg_t ec = {
            .ctrl_if = s_voice.ctrl_if, .gpio_if = s_voice.gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH, .pa_pin = BSP_POWER_AMP_IO, .use_mclk = true,
        };
        s_voice.codec_if = es8311_codec_new(&ec);
        if (!s_voice.data_if || !s_voice.ctrl_if || !s_voice.gpio_if || !s_voice.codec_if) {
            ui_chat_voice_set_status("Voice: codec init failed");
            goto fail;
        }
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = s_voice.codec_if, .data_if = s_voice.data_if,
        };
        s_voice.codec = esp_codec_dev_new(&dev_cfg);
        esp_codec_dev_sample_info_t si = {.bits_per_sample = 16, .channel = 1, .sample_rate = kSampleRate};
        esp_codec_dev_open(s_voice.codec, &si);
        esp_codec_dev_set_in_gain(s_voice.codec, 30.0f);
    }

    /* ---- ESP-SR AFE: VAD only; the music button starts every turn ---- */
    ESP_LOGI(TAG, "D_INIT: step5 VAD");
    ui_chat_voice_set_status("Voice: init VAD");
    {
        s_voice.models = esp_srmodel_init("model");
        afe_config_t *afe_cfg = afe_config_init("M", s_voice.models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
        afe_cfg->aec_init = false;
        afe_cfg->ns_init = false;
        afe_cfg->vad_init = true;
        afe_cfg->vad_mode = VAD_MODE_0;
        afe_cfg->vad_min_noise_ms = 100;
        afe_cfg->wakenet_init = false;
        afe_cfg->agc_init = false;
        afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        s_voice.afe_iface = esp_afe_handle_from_config(afe_cfg);
        s_voice.afe = s_voice.afe_iface ? s_voice.afe_iface->create_from_config(afe_cfg) : nullptr;
        ESP_LOGI(TAG, "D_INIT: step5b AFE=%p", s_voice.afe);
        afe_config_free(afe_cfg);
        if (!s_voice.afe) {
            ui_chat_voice_set_status("Voice: AFE create failed");
            goto fail;
        }
    }

    /* ---- Opus encoder ---- */
    ESP_LOGI(TAG, "D_INIT: step6 Opus enc");
    ui_chat_voice_set_status("Voice: init Opus enc");
    {
        esp_opus_enc_config_t opus_cfg = {
            .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K, .channel = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16, .bitrate = 24000,
            .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO, .complexity = 5,
            .enable_fec = false, .enable_dtx = true, .enable_vbr = true,
        };
        esp_opus_enc_open(&opus_cfg, sizeof(opus_cfg), &s_voice.opus_enc);
        int frame_bytes = 0;
        esp_opus_enc_get_frame_size(s_voice.opus_enc, &frame_bytes, &s_voice.opus_enc_out_sz);
    }

    /* ---- Start voice processing task ---- */
    ui_chat_voice_set_status("Voice: ready");
    xTaskCreate(voice_task, "xiaozhi_voice", 32768, nullptr, 4, nullptr);

    /* Init done — free this task's stack */
    vTaskDelete(NULL);

fail:
    ui_chat_voice_set_status("Voice: init failed");
    ESP_LOGE(TAG, "Hardware init deferred task failed");
    vTaskDelete(NULL);
}

static void ensure_uuid(void)
{
    /* Read UUID from NVS, or generate and persist it */
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_voice.uuid);
        nvs_get_str(h, "uuid", s_voice.uuid, &sz);
        nvs_close(h);
    }
    if (s_voice.uuid[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_voice.uuid, sizeof(s_voice.uuid),
                 "%02x%02x%02x-%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        /* Persist so future boots and OTA see the same UUID */
        if (nvs_open("board", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "uuid", s_voice.uuid);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "Generated and saved UUID: %s", s_voice.uuid);
    }
}

extern "C" esp_err_t voice_xiaozhi_start(void)
{
    ui_chat_voice_set_status("Voice: starting...");

    if (s_voice.events != nullptr) {
        return ESP_OK;
    }

    /* Ensure stable device identity before anything that talks to the server */
    ensure_uuid();

    s_voice.events = xEventGroupCreate();
    if (s_voice.events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(
        voice_hardware_init_task,
        "voice_init", 24576, nullptr, 3, nullptr);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

extern "C" esp_err_t voice_xiaozhi_start_listening(void)
{
    if (s_voice.events == nullptr || s_voice.afe == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_voice.manual_start_requested.store(true);
    return ESP_OK;
}

/* ---- OTA / device activation (background task, own stack) ---- */
extern "C" esp_err_t xiaozhi_ota_register(void)
{
    uint8_t raw_mac[6];
    get_wifi_mac(raw_mac);
    char mac[18] = {};
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             raw_mac[0], raw_mac[1], raw_mac[2], raw_mac[3], raw_mac[4], raw_mac[5]);

    /* Build device-info JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", mac);

    char uuid[40] = {};
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(uuid);
        nvs_get_str(h, "uuid", uuid, &sz);
        nvs_close(h);
    }
    if (uuid[0]) cJSON_AddStringToObject(root, "uuid", uuid);
    /* board must be a JSON object (the official GetBoardJson() convention) */
    cJSON *board_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(board_obj, "name", "esp32_p4_function_ev_board");
    cJSON_AddItemToObject(root, "board", board_obj);
    cJSON_AddStringToObject(root, "version", "2.4.0");
    {
        cJSON *chip = cJSON_AddObjectToObject(root, "chip");
        cJSON_AddStringToObject(chip, "model", "ESP32-P4");
        cJSON_AddNumberToObject(chip, "cores", 2);
    }
    {
        cJSON *flash = cJSON_AddObjectToObject(root, "flash");
        cJSON_AddNumberToObject(flash, "size", 16777216);
    }
    {
        cJSON *psram = cJSON_AddObjectToObject(root, "psram");
        cJSON_AddNumberToObject(psram, "size", 33554432);
    }
    {
        cJSON *app = cJSON_AddObjectToObject(root, "app");
        cJSON_AddStringToObject(app, "name", "xiaozhi");
        cJSON_AddStringToObject(app, "version", "2.4.0");
        cJSON_AddStringToObject(app, "idf_version", "v5.5.4");
        cJSON_AddStringToObject(app, "compile_time", __DATE__ " " __TIME__);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "OTA register: POST to OTA server (MAC=%s)", mac);
    ESP_LOGI(TAG, "OTA body: %s", body);

    esp_http_client_config_t cfg = {};
    cfg.url = "https://api.tenclass.net/xiaozhi/ota/";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", mac);
    if (uuid[0]) esp_http_client_set_header(client, "Client-Id", uuid);
    esp_http_client_set_header(client, "User-Agent", "esp-p4-function-ev-board/2.4.0");

    int body_len = strlen(body);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK) {
        esp_http_client_write(client, body, body_len);
        int64_t content_len = esp_http_client_fetch_headers(client);
        ESP_LOGI(TAG, "OTA status=%d content=%lld",
                 esp_http_client_get_status_code(client), content_len);

        if (content_len > 0) {
            char resp[2048] = {};
            int read_len = esp_http_client_read(client, resp, sizeof(resp) - 1);
            if (read_len > 0) {
                ESP_LOGI(TAG, "OTA response (len=%d): %.*s", read_len, read_len, resp);

                cJSON *r = cJSON_Parse(resp);
                if (r) {
                    cJSON *mq = cJSON_GetObjectItemCaseSensitive(r, "mqtt");
                    if (cJSON_IsObject(mq)) {
                        nvs_handle_t nvs;
                        if (nvs_open("mqtt", NVS_READWRITE, &nvs) == ESP_OK) {
                            cJSON *v;
                            v = cJSON_GetObjectItemCaseSensitive(mq, "endpoint");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "endpoint", v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(mq, "client_id");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "client_id", v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(mq, "username");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "username", v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(mq, "password");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "password", v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(mq, "publish_topic");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "publish_topic", v->valuestring);
                            nvs_commit(nvs);
                            nvs_close(nvs);
                            ESP_LOGI(TAG, "MQTT credentials saved to NVS");
                        }
                    }
                    /* WebSocket config (kept as fallback for server compat) */
                    cJSON *ws = cJSON_GetObjectItemCaseSensitive(r, "websocket");
                    if (cJSON_IsObject(ws)) {
                        nvs_handle_t nvs;
                        if (nvs_open("websocket", NVS_READWRITE, &nvs) == ESP_OK) {
                            cJSON *v;
                            v = cJSON_GetObjectItemCaseSensitive(ws, "url");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "url", v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(ws, "token");
                            if (cJSON_IsString(v)) nvs_set_str(nvs, "token", v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(ws, "version");
                            if (cJSON_IsNumber(v)) nvs_set_i32(nvs, "version", v->valueint);
                            nvs_commit(nvs);
                            nvs_close(nvs);
                            ESP_LOGI(TAG, "WebSocket credentials saved to NVS");
                        }
                    }
                    cJSON_Delete(r);
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "OTA HTTP open failed: %s", esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return err;
}
}
