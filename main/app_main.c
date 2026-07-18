#include "bsp/esp32_p4_function_ev_board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "ui.h"

#include <math.h>
#include <string.h>

#include "sdcard_init.h"
#include "pose_estimator.hpp"

static const char *TAG = "MAIN";

/* ============================================================
 * 帧缓冲 — 相机回调存帧，推理任务消费
 * ============================================================ */
#define MAX_FRAME_BYTES (1280 * 720 * 2)

static uint8_t *s_frame_buf = NULL;
static uint32_t s_frame_w = 0;
static uint32_t s_frame_h = 0;
static uint32_t s_frame_stride = 0;
static volatile bool s_frame_ready = false;
static TaskHandle_t s_infer_task_h = NULL;

/* ============================================================
 * 相机帧回调
 * ============================================================ */
static void on_camera_frame(const uint8_t *buf, uint32_t len,
                            uint32_t w, uint32_t h, uint32_t stride,
                            uint32_t fmt)
{
    (void)len;
    (void)fmt;

    if (s_frame_buf && stride * h <= MAX_FRAME_BYTES) {
        memcpy(s_frame_buf, buf, stride * h);
        s_frame_w = w;
        s_frame_h = h;
        s_frame_stride = stride;
        s_frame_ready = true;
        if (s_infer_task_h) {
            xTaskNotifyGive(s_infer_task_h);
        }
    }

    if (bsp_display_lock(0)) {
        ui_update_camera_preview(buf, w, h, stride);
        bsp_display_unlock();
    }
}

/* ============================================================
 * 姿态推理任务 (Core 0)
 * ============================================================ */
static void pose_inference_task(void *arg)
{
    (void)arg;
    float joints[UI_POSE_JOINT_COUNT][2];
    float confs[UI_POSE_JOINT_COUNT];
    float score;

    vTaskDelay(pdMS_TO_TICKS(1500));

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (!s_frame_ready) continue;
        s_frame_ready = false;

        esp_err_t ret = pose_estimator_run(
            s_frame_buf,
            s_frame_w, s_frame_h, s_frame_stride,
            joints, confs, &score);

        if (bsp_display_lock(20)) {
            if (ret == ESP_OK) {
                uint8_t dev[UI_POSE_JOINT_COUNT];
                for (int i = 0; i < UI_POSE_JOINT_COUNT; i++) {
                    dev[i] = (confs[i] < 0.3f) ? 2 : 0;
                }
                ui_update_skeleton(joints, UI_POSE_JOINT_COUNT, dev);
                ui_update_suggestion("姿态检测正常");
            } else {
                ui_update_suggestion("未检测到人体");
            }
            ui_update_fps(cam_is_running() ? cam_get_fps() : 0.0f);
            bsp_display_unlock();
        }
    }
}

/* ============================================================
 * 主入口
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "Initializing ESP32-P4 posture observation UI");

    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg = {
            .hdmi_resolution = BSP_HDMI_RES_NONE,
            .dsi_bus = {
                .phy_clk_src = 0,
                .lane_bit_rate_mbps = 500,
            },
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
        },
    };

    if (!bsp_display_start_with_config(&display_cfg)) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    bsp_display_backlight_on();



    esp_err_t sd_ret = sdcard_init();
    if (sd_ret == ESP_OK) {
        pose_estimator_load_and_print();
    } else {
        ESP_LOGW(TAG, "SD card unavailable, running in demo mode");
    }


    
    s_frame_buf = (uint8_t *)heap_caps_aligned_alloc(
        16, MAX_FRAME_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM frame buffer");
    }

    if (bsp_display_lock(-1)) {
        ui_init();
        bsp_display_unlock();
    }

    xTaskCreatePinnedToCore(pose_inference_task, "pose_infer",
                            16384, NULL, 5, &s_infer_task_h, 0);

#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t cam_ret = cam_start(1280, 720, 30, on_camera_frame);
    if (bsp_display_lock(-1)) {
        ui_set_system_status(cam_ret == ESP_OK
                             ? "摄像头：已连接"
                             : "摄像头：启动失败");
        bsp_display_unlock();
    }
    if (cam_ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera start failed: %s", esp_err_to_name(cam_ret));
    }
#endif
}
