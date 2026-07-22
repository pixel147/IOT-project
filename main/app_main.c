#include "bsp/esp32_p4_function_ev_board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "ui.h"
#include "sdcard_init.h"
#include "face_detect_wrapper.hpp"
#include "emotion_tflite.hpp"

#include <string.h>

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

/* ---- 相机回调 ---- */
static void on_camera_frame(const uint8_t *buf, uint32_t len,
                            uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt)
{
    (void)len; (void)fmt;
    s_frame_w = w;
    s_frame_h = h;
    s_frame_stride = stride;

    /* 乒乓写入捕获缓冲（无锁，不与 LVGL 竞争） */
    int idx = (s_ready_idx == -1 || s_ready_idx == 0) ? 1 : 0;
    if (s_fb_cap[idx] && stride * h <= FRAME_BYTES) {
        memcpy(s_fb_cap[idx], buf, stride * h);
        size_t align_sz = (stride * h + 63) & ~63;
        esp_cache_msync(s_fb_cap[idx], align_sz, ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        s_ready_idx = idx;
    }

    /*
     * 锁内同步渲染（参考 esp_brookesia_phone camera_video_frame_operation）：
     *   1. 获取 LVGL 锁（阻塞等待，确保不与 LVGL 任务竞争）
     *   2. 拷贝捕获帧到显示缓冲
     *   3. 设置 canvas → 立即触发 lv_refr_now（同步渲染到framebuffer）
     *   4. 释放锁 → 显示控制器扫描完整帧，零撕裂
     */
    if (lvgl_port_lock(-1)) {
        int ridx = s_ready_idx;
        if (ridx >= 0 && s_fb_cap[ridx] && s_fb_disp) {
            size_t align_sz = (stride * h + 63) & ~63;
            esp_cache_msync(s_fb_cap[ridx], align_sz,
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            memcpy(s_fb_disp, s_fb_cap[ridx], stride * h);
            esp_cache_msync(s_fb_disp, align_sz, ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

            /* 人脸检测 + 情绪识别 + 绘制 bbox */
            {
                static const uint8_t emo_colors[7][3] = {
                    {255,0,0},{0,140,100},{0,165,255},{0,255,0},
                    {255,0,0},{255,255,0},{180,180,180}};

                face_detect_results_t faces;
                face_detect_run((uint16_t *)s_fb_disp, w, h, &faces);

                for (int i = 0; i < faces.count; i++) {
                    int cls = 6; float conf = 0;
                    emotion_tflite_run((const uint8_t *)s_fb_disp,
                                       w, h, w * 2,
                                       faces.faces[i].x, faces.faces[i].y,
                                       faces.faces[i].w, faces.faces[i].h,
                                       &cls, &conf);

                    draw_rect_rgb565((uint16_t *)s_fb_disp, w, h,
                                     faces.faces[i].x, faces.faces[i].y,
                                     faces.faces[i].w, faces.faces[i].h,
                                     emo_colors[cls][0],
                                     emo_colors[cls][1],
                                     emo_colors[cls][2], 3);
                }
            }

            ui_update_camera_preview(s_fb_disp, w, h, stride);
            /* 强制 LVGL 立即渲染 — 关键！帧缓冲在锁内完成刷新 */
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
    if (s_fb_disp) {
        memset(s_fb_disp, 0, FRAME_BYTES);
        ui_update_camera_preview(s_fb_disp, CAM_WIDTH, CAM_HEIGHT, CAM_WIDTH * 2);
    }

    /* 3. 初始化 UI */
    lvgl_port_lock(-1);
    ui_init();
    ui_set_system_status("就绪");
    ui_update_suggestion("系统已启动，等待功能接入…");
    lv_timer_create(fps_timer_cb, 1000, NULL);
    lvgl_port_unlock();

    /* 4. 挂载 SD 卡 */
    ESP_LOGI(TAG, "Mounting SD card...");
    ui_set_system_status("SD 卡挂载中");
    esp_err_t sd_ret = sdcard_init();
    lvgl_port_lock(-1);
    if (sd_ret == ESP_OK) {
        ui_set_system_status("SD 卡：已就绪");
        /* 加载人脸检测 + 情绪识别模型 */
        face_detect_init();
        emotion_tflite_load();
    } else {
        ESP_LOGW(TAG, "SD card init failed");
        ui_set_system_status("SD 卡：失败");
    }
    lvgl_port_unlock();

    /* 5. 启动 MIPI-CSI 摄像头 */
    esp_err_t cam_ret = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_IDF_TARGET_ESP32P4
    cam_ret = cam_start(CAM_WIDTH, CAM_HEIGHT, 30, on_camera_frame);
#endif
    lvgl_port_lock(-1);
    ui_set_system_status(cam_ret == ESP_OK ? "摄像头：已连接" : "摄像头：失败");
    lvgl_port_unlock();

    ESP_LOGI(TAG, "System ready — camera preview + UI running");
}
