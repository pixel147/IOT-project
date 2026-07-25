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
#include "esp_mac.h"
#include "esp_opus_enc.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cJSON.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>

#include "chat.h"

namespace {

constexpr const char *TAG = "XIAOZHI_VOICE";
constexpr int kSampleRate = 16000;
constexpr int kOpusFrameSamples = 960;  // Official XiaoZhi protocol: 60 ms at 16 kHz.
constexpr EventBits_t kServerHello = BIT0;
constexpr EventBits_t kWebSocketConnected = BIT1;

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
    void *opus = nullptr;
    int opus_output_size = 0;
    esp_websocket_client_handle_t websocket = nullptr;
    EventGroupHandle_t events = nullptr;
    char session_id[96] = {};
    char url[256] = {};
    char token[256] = {};
    char uuid[96] = {};
    char device_id[18] = {};
    char headers[720] = {};
    int protocol_version = 1;
    std::atomic<bool> manual_start_requested{false};
    std::atomic<bool> ota_done{false};
    int post_wake_skip = 0;
    bool listening = false;
    int silence_frames = 0;
    std::vector<int16_t> opus_pcm;
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

static void stop_listening(const char *status)
{
    if (s_voice.listening && esp_websocket_client_is_connected(s_voice.websocket) && s_voice.session_id[0]) {
        char message[160];
        snprintf(message, sizeof(message),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
                 s_voice.session_id);
        esp_websocket_client_send_text(s_voice.websocket, message, strlen(message), portMAX_DELAY);
    }
    s_voice.listening = false;
    s_voice.silence_frames = 0;
    s_voice.opus_pcm.clear();
    s_voice.afe_iface->enable_wakenet(s_voice.afe);
    ui_chat_voice_set_status(status);
}

static void send_opus_frame(const int16_t *pcm)
{
    if (!s_voice.listening || !esp_websocket_client_is_connected(s_voice.websocket)) {
        return;
    }

    std::vector<uint8_t> output(s_voice.opus_output_size);
    esp_audio_enc_in_frame_t input = {
        .buffer = reinterpret_cast<uint8_t *>(const_cast<int16_t *>(pcm)),
        .len = static_cast<uint32_t>(kOpusFrameSamples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t encoded = {
        .buffer = output.data(),
        .len = static_cast<uint32_t>(output.size()),
        .encoded_bytes = 0,
    };
    if (esp_opus_enc_process(s_voice.opus, &input, &encoded) == ESP_AUDIO_ERR_OK &&
        encoded.encoded_bytes > 0) {
        esp_websocket_client_send_bin(s_voice.websocket, reinterpret_cast<const char *>(output.data()),
                                      encoded.encoded_bytes, portMAX_DELAY);
    }
}

static void websocket_event(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_websocket_event_data_t *>(event_data);
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        xEventGroupSetBits(s_voice.events, kWebSocketConnected);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGI(TAG, "WS_EVENT: ERROR");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_voice.listening = false;
        s_voice.session_id[0] = '\0';
        ESP_LOGI(TAG, "WS_EVENT: DISCONNECTED");
        ui_chat_voice_set_status("Voice: disconnected");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DATA && event && event->op_code == WS_TRANSPORT_OPCODES_TEXT) {
        ESP_LOGI(TAG, "WS_EVENT: TEXT len=%d: %.*s", event->data_len, event->data_len, event->data_ptr);
    } else if (event_id == WEBSOCKET_EVENT_DATA && event && event->op_code == WS_TRANSPORT_OPCODES_BINARY) {
        ESP_LOGI(TAG, "WS_EVENT: DATA (binary) len=%d", event->data_len);
        return;
    } else {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(event->data_ptr, event->data_len);
    if (!root) {
        ESP_LOGW(TAG, "Ignoring invalid XiaoZhi JSON frame");
        return;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0) {
        cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsString(session)) {
            strlcpy(s_voice.session_id, session->valuestring, sizeof(s_voice.session_id));
            xEventGroupSetBits(s_voice.events, kServerHello);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            ui_chat_voice_append_user(text->valuestring);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "tts") == 0) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        /* Only append on sentence_end or stop to avoid duplicate display */
        if (cJSON_IsString(state) && cJSON_IsString(text) && text->valuestring[0]) {
            if (strcmp(state->valuestring, "sentence_end") == 0 ||
                strcmp(state->valuestring, "stop") == 0) {
                ui_chat_voice_append_assistant(text->valuestring);
            }
        }
        if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0) {
            stop_listening("Voice: ready");
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        /* Server-side MCP handshake (Model Context Protocol for capabilities). */
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *method = payload ? cJSON_GetObjectItemCaseSensitive(payload, "method") : nullptr;
        cJSON *msg_id = payload ? cJSON_GetObjectItemCaseSensitive(payload, "id") : nullptr;
        if (cJSON_IsString(method) && strcmp(method->valuestring, "initialize") == 0) {
            int id = cJSON_IsNumber(msg_id) ? msg_id->valueint : 1;
            char mcp_resp[512] = {};
            snprintf(mcp_resp, sizeof(mcp_resp),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
                     "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                     "\"serverInfo\":{\"name\":\"esp32-p4\",\"version\":\"2.4.0\"}}},"
                     "\"session_id\":\"%s\"}",
                     id, s_voice.session_id);
            esp_websocket_client_send_text(s_voice.websocket, mcp_resp, strlen(mcp_resp), portMAX_DELAY);
            ESP_LOGI(TAG, "MCP initialize responded");
        } else if (cJSON_IsString(method) && strcmp(method->valuestring, "tools/list") == 0) {
            int id = cJSON_IsNumber(msg_id) ? msg_id->valueint : 2;
            char mcp_resp[256] = {};
            snprintf(mcp_resp, sizeof(mcp_resp),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
                     "\"result\":{\"tools\":[]}},\"session_id\":\"%s\"}",
                     id, s_voice.session_id);
            esp_websocket_client_send_text(s_voice.websocket, mcp_resp, strlen(mcp_resp), portMAX_DELAY);
            ESP_LOGI(TAG, "MCP tools/list responded (empty)");
        }
    }
    cJSON_Delete(root);
}

static void ws_close(void)
{
    if (s_voice.websocket) {
        esp_websocket_client_stop(s_voice.websocket);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_destroy(s_voice.websocket);
        s_voice.websocket = nullptr;
    }
}

static bool open_xiaozhi_session(const char *wake_word)
{
    ws_close();
    xEventGroupClearBits(s_voice.events, kServerHello | kWebSocketConnected);
    s_voice.session_id[0] = '\0';

    ESP_LOGI(TAG, "WS connecting: url=%s token=%s", s_voice.url, s_voice.token);
    snprintf(s_voice.headers, sizeof(s_voice.headers),
             "Authorization: Bearer %s\r\nProtocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
             s_voice.token, s_voice.protocol_version, s_voice.device_id, s_voice.uuid);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = s_voice.url;
    cfg.headers = s_voice.headers;
    cfg.buffer_size = 2048;
    cfg.task_stack = 8192;
    cfg.disable_auto_reconnect = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    s_voice.websocket = esp_websocket_client_init(&cfg);
    if (!s_voice.websocket) return false;

    esp_websocket_register_events(s_voice.websocket, WEBSOCKET_EVENT_ANY, websocket_event, nullptr);
    esp_err_t err = esp_websocket_client_start(s_voice.websocket);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "WS start failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_voice.events, kWebSocketConnected, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & kWebSocketConnected)) {
        ESP_LOGI(TAG, "WS connect timed out (10s)");
        ui_chat_voice_set_status("Voice: conn timed out");
        return false;
    }
    ESP_LOGI(TAG, "WS connected OK");

    char hello[384];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"version\":%d,\"transport\":\"websocket\","
             "\"features\":{\"mcp\":true,\"aec\":false,\"vad\":true},"
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}",
             s_voice.protocol_version);
    esp_websocket_client_send_text(s_voice.websocket, hello, strlen(hello), portMAX_DELAY);

    bits = xEventGroupWaitBits(s_voice.events, kServerHello, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    bool ok = (bits & kServerHello) != 0;
    ESP_LOGI(TAG, "WS hello %s", ok ? "received OK" : "timed out (5s)");
    return ok;
}

static void voice_task(void *arg)
{
    (void)arg;

    while (1) {  /* Outer loop: reconnect forever */
        /* OTA registration once per boot (background task, HTTPS) */
        if (!s_voice.ota_done.exchange(true)) {
            ESP_LOGI(TAG, "Starting OTA registration...");
            esp_err_t oret = xiaozhi_ota_register();
            ESP_LOGI(TAG, "OTA registration: %s", esp_err_to_name(oret));
            /* Refresh URL/token from NVS (OTA may have updated them) */
            nvs_handle_t ws_nvs;
            if (nvs_open("websocket", NVS_READONLY, &ws_nvs) == ESP_OK) {
                size_t sz = sizeof(s_voice.url);
                nvs_get_str(ws_nvs, "url", s_voice.url, &sz);
                sz = sizeof(s_voice.token);
                nvs_get_str(ws_nvs, "token", s_voice.token, &sz);
                nvs_close(ws_nvs);
            }
        }

        /* Settle delay before WebSocket connect */
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!open_xiaozhi_session("xiaozhi")) {
            /* Retry with short delays; user button can break out early */
            ui_chat_voice_set_status("Voice: retry in 3s...");
            for (int i = 0; i < 6; i++) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (s_voice.manual_start_requested.exchange(false)) {
                    break;  /* User clicked → retry immediately */
                }
            }
            continue;
        }

        /* Wait for wake word */
        ui_chat_voice_set_status("Voice: say xiaozhixiaozhi");

        while (s_voice.websocket && esp_websocket_client_is_connected(s_voice.websocket)) {
            /* Check for manual listening request */
            if (s_voice.manual_start_requested.exchange(false)) {
                char listen[192];
                snprintf(listen, sizeof(listen),
                         "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\","
                         "\"session_id\":\"%s\"}", s_voice.session_id);
                if (s_voice.websocket) esp_websocket_client_send_text(s_voice.websocket, listen, strlen(listen), portMAX_DELAY);
                s_voice.listening = true;
                s_voice.silence_frames = 0;
                s_voice.opus_pcm.clear();
                ui_chat_voice_set_status("Voice: listening...");
            }

            /* Read audio from codec and feed to AFE pipeline (always, for wake word detection) */
            if (!s_voice.afe_iface || !s_voice.afe || !s_voice.codec) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            /* Pre-allocated buffer for AFE feeding (avoids heap alloc per iteration) */
            static int16_t *afe_buf = nullptr;
            static int afe_buf_size = 0;
            int feed_chunk = s_voice.afe_iface->get_feed_chunksize(s_voice.afe);
            int needed = feed_chunk * (int)sizeof(int16_t);
            if (!afe_buf || afe_buf_size < needed) {
                if (afe_buf) heap_caps_free(afe_buf);
                afe_buf = (int16_t *)heap_caps_malloc(needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                afe_buf_size = afe_buf ? needed : 0;
                if (!afe_buf) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
            }
            /* Read PCM directly from I2S RX (codec_dev_read returns 0 on this board) */
            size_t i2s_bytes = 0;
            esp_err_t i2s_ret = i2s_channel_read(s_voice.rx, afe_buf, needed, &i2s_bytes, pdMS_TO_TICKS(50));
            if (i2s_ret == ESP_OK && i2s_bytes > 0) {
                s_voice.afe_iface->feed(s_voice.afe, afe_buf);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            /* Fetch processed audio from AFE (always, to check wake word trigger) */
            afe_fetch_result_t *result = s_voice.afe_iface->fetch(s_voice.afe);
            if (!result) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            /* Check for wake word trigger event */
            if (result->wakeup_state > 0 && !s_voice.listening) {
                ESP_LOGI(TAG, "Wake word triggered! state=%d", result->wakeup_state);
                char listen[192];
                snprintf(listen, sizeof(listen),
                         "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\","
                         "\"session_id\":\"%s\"}", s_voice.session_id);
                if (s_voice.websocket) esp_websocket_client_send_text(s_voice.websocket, listen, strlen(listen), portMAX_DELAY);
                s_voice.listening = true;
                s_voice.silence_frames = 0;
                s_voice.opus_pcm.clear();
                s_voice.post_wake_skip = 5;  /* Drop ~300ms to avoid sending wake-word audio */
                ui_chat_voice_set_status("Voice: wake word detected");
            }
            /* Only send audio data when actively listening */
            if (!s_voice.listening || result->data_size == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            /* Skip a few frames after wake word to avoid sending wake-word audio */
            if (s_voice.post_wake_skip > 0) {
                s_voice.post_wake_skip--;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            /* Opus-encode and send */
            const size_t samples = result->data_size / sizeof(int16_t);
            s_voice.opus_pcm.insert(s_voice.opus_pcm.end(), result->data, result->data + samples);
            while (s_voice.opus_pcm.size() >= kOpusFrameSamples) {
                send_opus_frame(s_voice.opus_pcm.data());
                s_voice.opus_pcm.erase(s_voice.opus_pcm.begin(),
                                       s_voice.opus_pcm.begin() + kOpusFrameSamples);
            }
            s_voice.silence_frames = result->vad_state == VAD_SILENCE ? s_voice.silence_frames + 1 : 0;
            if (s_voice.silence_frames > 30) {
                stop_listening("Voice: processing");
            }
        }

        /* Inner loop exited => connection lost. Clean up and retry. */
        ESP_LOGI(TAG, "WS disconnected, reconnecting...");
        ui_chat_voice_set_status("Voice: reconnecting...");
        s_voice.listening = false;
        s_voice.session_id[0] = '\0';
        ws_close();
    }
}


/* ---- hardware init task (deferred to background) ---- */
static void voice_hardware_init_task(void *arg)
{
    (void)arg;

    /* ---- Mount NVS & load XiaoZhi credentials ---- */
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

    ui_chat_voice_set_status("Voice: reading credentials");

    /* Load credentials from NVS */
    {
        nvs_handle_t ws;
        esp_err_t err = nvs_open("websocket", NVS_READONLY, &ws);
        if (err != ESP_OK) {
            ui_chat_voice_set_status("Voice: activate XiaoZhi first");
            goto fail;
        }
        size_t sz = sizeof(s_voice.url);
        err = nvs_get_str(ws, "url", s_voice.url, &sz);
        if (err == ESP_OK) {
            sz = sizeof(s_voice.token);
            err = nvs_get_str(ws, "token", s_voice.token, &sz);
        }
        if (err == ESP_OK) {
            int32_t ver = 1;
            nvs_get_i32(ws, "version", &ver);
            s_voice.protocol_version = ver > 0 ? ver : 1;
        }
        nvs_close(ws);
        if (err != ESP_OK) {
            ui_chat_voice_set_status("Voice: missing credentials");
            goto fail;
        }
    }
    {
        nvs_handle_t brd;
        if (nvs_open("board", NVS_READONLY, &brd) == ESP_OK) {
            size_t sz = sizeof(s_voice.uuid);
            nvs_get_str(brd, "uuid", s_voice.uuid, &sz);
            nvs_close(brd);
        }
    }
    /* Fallback: generate a device UUID from MAC if not stored in NVS */
    if (s_voice.uuid[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_voice.uuid, sizeof(s_voice.uuid),
                 "%02x%02x%02x-%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Generated UUID from MAC: %s", s_voice.uuid);
    }

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

    /* ---- ESP-SR: WakeNet + AFE ---- */
    ESP_LOGI(TAG, "D_INIT: step5 ESP-SR");
    ui_chat_voice_set_status("Voice: load WakeNet");
    {
        s_voice.models = esp_srmodel_init("model");
        char *wn = s_voice.models ? esp_srmodel_filter(s_voice.models, ESP_WN_PREFIX, nullptr) : nullptr;
        if (!wn) {
            ui_chat_voice_set_status("Voice: WakeNet model unavailable");
            goto fail;
        }
        afe_config_t *afe_cfg = afe_config_init("M", s_voice.models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
        afe_cfg->aec_init = false;
        afe_cfg->ns_init = false;
        afe_cfg->vad_init = true;
        afe_cfg->vad_mode = VAD_MODE_0;
        afe_cfg->vad_min_noise_ms = 100;
        afe_cfg->wakenet_init = true;
        afe_cfg->wakenet_model_name = wn;
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
    ESP_LOGI(TAG, "D_INIT: step6 Opus");
    ui_chat_voice_set_status("Voice: init Opus");
    {
        esp_opus_enc_config_t opus_cfg = {
            .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K, .channel = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16, .bitrate = 24000,
            .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO, .complexity = 5,
            .enable_fec = false, .enable_dtx = true, .enable_vbr = true,
        };
        esp_opus_enc_open(&opus_cfg, sizeof(opus_cfg), &s_voice.opus);
        int frame_bytes = 0;
        esp_opus_enc_get_frame_size(s_voice.opus, &frame_bytes, &s_voice.opus_output_size);
    }
    ESP_LOGI(TAG, "D_INIT: delay before exit");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "D_INIT: step7 creating voice_task");

    /* ---- Start voice processing task ---- */
    ui_chat_voice_set_status("Voice: say xiaozhixiaozhi");
    ESP_LOGI(TAG, "D_INIT: step7b creating voice_task...");
    xTaskCreate(voice_task, "xiaozhi_voice", 32768, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "D_INIT: sleeping forever to avoid exit crash");
    while (1) { vTaskDelay(pdMS_TO_TICKS(30000)); }

fail:
    ui_chat_voice_set_status("Voice: init failed");
    ESP_LOGE(TAG, "Hardware init deferred task failed");
    while (1) { vTaskDelay(pdMS_TO_TICKS(30000)); }
}

extern "C" esp_err_t voice_xiaozhi_start(void)
{
    ui_chat_voice_set_status("Voice: starting...");

    if (s_voice.events != nullptr) {
        return ESP_OK;
    }

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
    cJSON_AddStringToObject(root, "board", "esp-p4-function-ev-board");
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
        cJSON_AddStringToObject(app, "name", "emotion_chat");
        cJSON_AddStringToObject(app, "version", "2.4.0");
        cJSON_AddStringToObject(app, "idf_version", "v5.5.4");
        cJSON_AddStringToObject(app, "compile_time", __DATE__ " " __TIME__);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "OTA register: POST to OTA server (MAC=%s)", mac);

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

                /* Parse response for websocket config */
                cJSON *r = cJSON_Parse(resp);
                if (r) {
                    cJSON *ws = cJSON_GetObjectItemCaseSensitive(r, "websocket");
                    if (cJSON_IsObject(ws)) {
                        nvs_handle_t nvs;
                        if (nvs_open("websocket", NVS_READWRITE, &nvs) == ESP_OK) {
                            cJSON *val;
                            val = cJSON_GetObjectItemCaseSensitive(ws, "url");
                            if (cJSON_IsString(val))
                                nvs_set_str(nvs, "url", val->valuestring);
                            val = cJSON_GetObjectItemCaseSensitive(ws, "token");
                            if (cJSON_IsString(val))
                                nvs_set_str(nvs, "token", val->valuestring);
                            val = cJSON_GetObjectItemCaseSensitive(ws, "version");
                            if (cJSON_IsNumber(val))
                                nvs_set_i32(nvs, "version", val->valueint);
                            nvs_commit(nvs);
                            nvs_close(nvs);
                            ESP_LOGI(TAG, "WebSocket credentials saved to NVS");
                        }
                    } else {
                        ESP_LOGW(TAG, "No websocket config in OTA response");
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
