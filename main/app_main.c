#include "bsp/esp32_p4_function_ev_board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "ui.h"
#include "home.h"
#include "settings.h"
#include "sdcard_init.h"
#include "face_detect_wrapper.hpp"
#include "emotion_espdl.hpp"
#include "wifi.h"
#include "llm_client.h"

#include <string.h>

/* ---- 凭据配置 ---- */
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASSWORD   "your_wifi_password"
#define LLM_API_KEY     "sk-your-api-key"
#define LLM_BASE_URL    LLM_BASE_DEEPSEEK
#define LLM_MODEL_NAME  LLM_MODEL_DEEPSEEK_CHAT

static const char *TAG = "MAIN";

#define CAM_WIDTH   640
#define CAM_HEIGHT  480
#define FRAME_BYTES (640 * 480 * 2)

/*
 * 三缓冲防撕裂：
 *   s_fb_cap[2] — 乒乓捕获缓冲（相机写入，无锁）
 *   s_fb_disp   — 专用显示缓冲（仅在 LVGL 锁内写入，canvas 恒定指向）
 *
 * 相机自由乒乓写入 s_fb_cap[]；LVGL 渲染时始终从 s_fb_disp 读取，
 * 两者永不冲突。s_fb_disp 的更新在 LVGL 锁内完成，保证渲染原子性。
 */
static uint8_t *s_fb_cap[2] = {NULL, NULL};
static uint8_t *s_fb_disp = NULL;
static volatile int s_ready_idx = -1;
static uint32_t s_frame_w = 0, s_frame_h = 0, s_frame_stride = 0;

/* ---- 共享推理结果 ---- */
static face_detect_results_t s_ai_faces;
static int s_ai_classes[FACE_DETECT_MAX_FACES];
static float s_ai_confs[FACE_DETECT_MAX_FACES];

/* ---- 多屏幕管理 ---- */
static lv_obj_t *s_home_scr = NULL;
static lv_obj_t *s_monitor_scr = NULL;
static lv_obj_t *s_settings_scr = NULL;

static void on_icon_monitor(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(-1)) {
        lv_screen_load(s_monitor_scr);
        lvgl_port_unlock();
    }
}
static void on_icon_chat(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(-1)) {
        ui_show_message("即将上线", "AI 聊天功能正在开发中，敬请期待…", 3000);
        lvgl_port_unlock();
    }
}
static void on_icon_settings(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(-1)) {
        lv_screen_load(s_settings_scr);
        ui_settings_scan_wifi();
        lvgl_port_unlock();
    }
}
static void on_monitor_back(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(-1)) {
        lv_screen_load(s_home_scr);
        lvgl_port_unlock();
    }
}
static void on_settings_back(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(-1)) {
        lv_screen_load(s_home_scr);
        lvgl_port_unlock();
    }
}

/* ---- 相机回调（拷贝帧 + 人脸检测 + 画框） ---- */
static void on_camera_frame(const uint8_t *buf, uint32_t len,
                            uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt)
{
    (void)len; (void)fmt;
    s_frame_w = w;
    s_frame_h = h;
    s_frame_stride = stride;

    int idx = (s_ready_idx == -1 || s_ready_idx == 0) ? 1 : 0;
    if (s_fb_cap[idx] && stride * h <= FRAME_BYTES) {
        memcpy(s_fb_cap[idx], buf, stride * h);
        size_t align_sz = (stride * h + 63) & ~63;
        esp_cache_msync(s_fb_cap[idx], align_sz, ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        s_ready_idx = idx;
    }

    if (!s_fb_disp) return;

    if (lvgl_port_lock(-1)) {
        int ridx = s_ready_idx;
        if (ridx >= 0 && s_fb_cap[ridx]) {
            size_t align_sz = (stride * h + 63) & ~63;
            esp_cache_msync(s_fb_cap[ridx], align_sz,
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            memcpy(s_fb_disp, s_fb_cap[ridx], stride * h);

            /* 人脸检测（直接在当前帧上做，与原始架构一致） */
            face_detect_results_t faces = {0};
            face_detect_run((uint16_t *)s_fb_disp, w, h, &faces);

            /* 绘制人脸框（s_ai_classes 由 emotion_task 更新） */
            static const uint8_t emo_colors[7][3] = {
                {255,0,0},{0,140,100},{0,165,255},{0,255,0},
                {255,0,0},{255,255,0},{180,180,180}};

            for (int i = 0; i < faces.count; i++) {
                draw_rect_rgb565((uint16_t *)s_fb_disp, w, h,
                                 faces.faces[i].x, faces.faces[i].y,
                                 faces.faces[i].w, faces.faces[i].h,
                                 emo_colors[s_ai_classes[i]][0],
                                 emo_colors[s_ai_classes[i]][1],
                                 emo_colors[s_ai_classes[i]][2], 3);
            }

            /* 情绪徽章 */
            if (faces.count > 0) {
                ui_update_emotion(s_ai_classes[0], s_ai_confs[0]);
            } else {
                ui_hide_emotion();
            }

            /* 更新共享人脸结果供情绪任务使用 */
            s_ai_faces = faces;
            __sync_synchronize();

            ui_update_camera_preview(s_fb_disp, w, h, stride);
            lv_refr_now(NULL);
        }
        lvgl_port_unlock();
    }
}

/* ---- FPS 定时器 ---- */
static void fps_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_update_fps(cam_get_fps());
}

/* ---- 情绪识别任务（拷贝帧到本地缓冲再推理，避免读竞争） ---- */
static void emotion_task(void *arg)
{
    (void)arg;
    uint8_t *roi_buf = (uint8_t *)heap_caps_aligned_alloc(
        32, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!roi_buf) { ESP_LOGE(TAG, "roi_buf alloc failed"); vTaskDelete(NULL); }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (!s_fb_disp || s_frame_w == 0) continue;

        __sync_synchronize();
        int n_faces = s_ai_faces.count;
        if (n_faces == 0) continue;

        /* 拷贝完整帧到本地缓冲，确保推理期间数据不被相机覆写 */
        size_t align_sz = (s_frame_stride * s_frame_h + 63) & ~63;
        esp_cache_msync(s_fb_disp, align_sz, ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        memcpy(roi_buf, s_fb_disp, s_frame_stride * s_frame_h);

        for (int i = 0; i < n_faces && i < FACE_DETECT_MAX_FACES; i++) {
            int64_t t0 = esp_timer_get_time();
            emotion_espdl_run((const uint8_t *)roi_buf,
                               s_frame_w, s_frame_h, s_frame_w * 2,
                               s_ai_faces.faces[i].x, s_ai_faces.faces[i].y,
                               s_ai_faces.faces[i].w, s_ai_faces.faces[i].h,
                               &s_ai_classes[i], &s_ai_confs[i]);
            int64_t dt = esp_timer_get_time() - t0;
            ESP_LOGI("LAT", "emotion[%d]: %lld us  cls=%d conf=%.2f",
                     i, dt, s_ai_classes[i], s_ai_confs[i]);
        }
    }
}

/* ---- 情绪提供者回调（Pull 模式：LLM 每次 chat 前实时拉取） ---- */
static void provide_emotion(int *cls, float *conf)
{
    *cls  = s_ai_classes[0];
    *conf = s_ai_confs[0];
}

/* ---- LLM 建议任务（每30秒生成陪伴提示） ---- */
#define LLM_SUGGESTION_INTERVAL_MS 30000
#define LLM_PROMPT_BUF_SIZE 256
#define LLM_RESP_BUF_SIZE   512

static void llm_suggestion_task(void *arg)
{
    (void)arg;
    char prompt[LLM_PROMPT_BUF_SIZE];
    char response[LLM_RESP_BUF_SIZE];

    /* 等待 LLM 初始化完成 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (true) {
        /* 构造情绪感知提示词 */
        int cls = s_ai_classes[0];
        float conf = s_ai_confs[0];
        const char *emo_name = "未知";
        if (cls >= 0 && cls < 7) {
            emo_name = LLM_EMOTION_NAMES[cls];
        }
        snprintf(prompt, sizeof(prompt),
                 "用户当前情绪是\"%s\"（置信度%d%%）。"
                 "请根据这个情绪，用1-2句简短温暖的话给出陪伴建议（不超过25字）",
                 emo_name, (int)(conf * 100 + 0.5f));

        response[0] = '\0';
        esp_err_t ret = llm_chat(prompt, response, sizeof(response));

        /* 更新建议栏（需 LVGL 锁） */
        if (lvgl_port_lock(-1)) {
            if (ret == ESP_OK && response[0]) {
                ui_update_suggestion(response);
            } else {
                ui_update_suggestion("正在连接大模型…");
            }
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(LLM_SUGGESTION_INTERVAL_MS));
    }
}

/* ---- 主函数 ---- */
void app_main(void)
{
    ESP_LOGI(TAG, "Emotion + LLM Chat Terminal starting");

    /* 1. 初始化显示 */
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = (1024 * 200),
        .double_buffer = 1,
        .hw_cfg = {
            .hdmi_resolution = BSP_HDMI_RES_NONE,
            .dsi_bus = { .phy_clk_src = 0, .lane_bit_rate_mbps = 500 },
        },
        .flags = {
            .buff_dma    = true,
            .buff_spiram = true,
            /* sw_rotate disabled — extra buffer copy adds latency, worsens tearing */
        },
    };

    if (!bsp_display_start_with_config(&display_cfg)) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_backlight_on();

    /* 2. 分配三缓冲：2 乒乓捕获 + 1 显示 */
    for (int i = 0; i < 2; i++) {
        s_fb_cap[i] = heap_caps_aligned_alloc(64, FRAME_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_fb_cap[i]) {
            ESP_LOGE(TAG, "PSRAM cap fb[%d] alloc failed", i);
        }
    }
    s_fb_disp = heap_caps_aligned_alloc(64, FRAME_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_fb_disp) {
        ESP_LOGE(TAG, "PSRAM disp fb alloc failed");
    }

    /* 初始化 canvas 指向显示缓冲（只设一次，后续直接 memcpy + invalidate） */

    /* 3. 创建 Home + Monitor 屏幕，默认加载 Home */
    lvgl_port_lock(-1);

    /* Home */
    s_home_scr = ui_home_create();
    lv_screen_load(s_home_scr);

    /* Monitor（先创建但不加载） */
    s_monitor_scr = ui_create_monitor_screen();
    lv_timer_create(fps_timer_cb, 1000, NULL);

    /* Home 图标回调 */
    ui_home_set_icon_callback(0, on_icon_monitor);
    ui_home_set_icon_callback(1, on_icon_chat);
    ui_home_set_icon_callback(2, on_icon_settings);

    /* Monitor 返回 → Home */
    ui_monitor_set_back_callback(on_monitor_back);

    /* Settings 屏幕 */
    s_settings_scr = ui_settings_create();
    ui_settings_set_back_callback(on_settings_back);

    /* 给 canvas 设初始缓冲 */
    if (s_fb_disp) {
        memset(s_fb_disp, 0, FRAME_BYTES);
        ui_update_camera_preview(s_fb_disp, CAM_WIDTH, CAM_HEIGHT, CAM_WIDTH * 2);
    }

    lvgl_port_unlock();

    /* 4. 挂载 SD 卡 */
    ESP_LOGI(TAG, "Mounting SD card...");
    ui_set_system_status("SD 卡挂载中");
    esp_err_t sd_ret = sdcard_init();
    lvgl_port_lock(-1);
    if (sd_ret == ESP_OK) {
        ui_set_system_status("SD 卡：已就绪");
        face_detect_init();
        emotion_espdl_load();
    } else {
        ESP_LOGW(TAG, "SD card init failed");
        ui_set_system_status("SD 卡：失败");
    }
    lvgl_port_unlock();

    /* 5. 启动 WiFi（非阻塞，立即返回） */
    ESP_LOGI(TAG, "Connecting WiFi to %s…", WIFI_SSID);
    wifi_connect(WIFI_SSID, WIFI_PASSWORD);

    /* 6. 启动 MIPI-CSI 摄像头 */
    esp_err_t cam_ret = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_IDF_TARGET_ESP32P4
    cam_ret = cam_start(CAM_WIDTH, CAM_HEIGHT, 30, on_camera_frame);
#endif
    lvgl_port_lock(-1);
    ui_set_system_status(cam_ret == ESP_OK ? "摄像头：已连接" : "摄像头：失败");
    lvgl_port_unlock();

    /* 7. 等 WiFi 连接（后台进行，不阻塞摄像头） */
    ui_set_system_status("WiFi 连接中…");
    esp_err_t wifi_ret = wifi_wait_connected(15000);
    lvgl_port_lock(-1);
    if (wifi_ret == ESP_OK) {
        ui_set_system_status("WiFi：已连接");
        ESP_LOGI(TAG, "WiFi connected");

        /* 初始化大模型 */
        llm_config_t llm_cfg = {
            .base_url     = LLM_BASE_URL,
            .api_key      = LLM_API_KEY,
            .model        = LLM_MODEL_NAME,
            .max_tokens   = LLM_DEFAULT_MAX_TOKENS,
            .temperature  = LLM_DEFAULT_TEMPERATURE,
            .timeout_ms   = LLM_DEFAULT_TIMEOUT_MS,
        };
        esp_err_t llm_ret = llm_init(&llm_cfg);
        if (llm_ret == ESP_OK) {
            /* 注册情绪提供者（Pull 模式：每次 chat 前实时拉取） */
            llm_set_emotion_provider(provide_emotion);
            ui_set_system_status("大模型：已就绪");
            ESP_LOGI(TAG, "LLM ready: %s", LLM_MODEL_NAME);
        } else {
            ui_set_system_status("大模型：初始化失败");
            ESP_LOGW(TAG, "LLM init failed: %d", llm_ret);
        }
    } else {
        ui_set_system_status("WiFi：连接失败");
        ESP_LOGW(TAG, "WiFi timeout or failed");
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "System ready — camera preview + UI running");

    /* 8. 启动情绪识别任务（独立于相机回调，每2秒一次） */
    xTaskCreatePinnedToCore(emotion_task, "emotion", 16384, NULL, 3, NULL, 0);

    /* 9. 启动 LLM 建议任务（每30秒生成陪伴提示） */
    xTaskCreatePinnedToCore(llm_suggestion_task, "llm_suggest", 12288, NULL, 2, NULL, 0);
}
