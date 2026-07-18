#include "bsp/esp32_p4_function_ev_board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ui.h"

#include <math.h>

#include "sdcard_init.h"
#include "pose_estimator.hpp"

static const char *TAG = "MAIN";

/* 纯 UI 占位任务，后续替换为姿态模型任务 */
static float demo_joints[UI_POSE_JOINT_COUNT][2] = {
    { 0.00f,  0.76f}, {-0.08f,  0.80f}, { 0.08f,  0.80f},
    {-0.15f,  0.76f}, { 0.15f,  0.76f}, {-0.24f,  0.48f},
    { 0.24f,  0.48f}, {-0.42f,  0.20f}, { 0.42f,  0.20f},
    {-0.48f, -0.06f}, { 0.48f, -0.06f}, {-0.20f,  0.00f},
    { 0.20f,  0.00f}, {-0.24f, -0.42f}, { 0.24f, -0.42f},
    {-0.28f, -0.78f}, { 0.28f, -0.78f},
};

static void on_camera_frame(const uint8_t *buf, uint32_t len,
                            uint32_t w, uint32_t h, uint32_t stride,
                            uint32_t fmt)
{
    (void)len;
    (void)fmt;

    if (bsp_display_lock(0)) {
        ui_update_camera_preview(buf, w, h, stride);
        bsp_display_unlock();
    }
}

static void demo_pose_task(void *arg)
{
    (void)arg;
    float phase = 0.0f;

    while (true) {
        phase += 0.04f;
        demo_joints[7][1] = 0.20f + 0.03f * sinf(phase);
        demo_joints[8][1] = 0.20f + 0.03f * sinf(phase);

        if (bsp_display_lock(20)) {
            /* 仅模拟骨架，指标和指导保持空值 */
            ui_update_skeleton(demo_joints, UI_POSE_JOINT_COUNT, NULL);
            ui_update_fps(cam_is_running() ? cam_get_fps() : 0.0f);
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

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


    // =======================================================
    esp_err_t sd_ret = sdcard_init();
    if (sd_ret == ESP_OK) {
        pose_estimator_load_and_print();
    } else {
        ESP_LOGW(TAG, "SD card unavailable, running in demo mode");
    }
    // =======================================================




    if (bsp_display_lock(-1)) {
        ui_init();
        bsp_display_unlock();
    }

    xTaskCreatePinnedToCore(demo_pose_task, "demo_pose", 4096, NULL, 5, NULL, 1);

#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t cam_ret = cam_start(1280, 720, 30, on_camera_frame);
    if (bsp_display_lock(-1)) {
        ui_set_system_status(cam_ret == ESP_OK ? "摄像头：已连接" : "摄像头：启动失败");
        bsp_display_unlock();
    }
    if (cam_ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera start failed: %s", esp_err_to_name(cam_ret));
    }
#endif
}
