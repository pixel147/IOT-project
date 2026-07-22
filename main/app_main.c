#include "bsp/esp32_p4_function_ev_board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "ui.h"
#include "sdcard_init.h"

#include <string.h>

static const char *TAG = "MAIN";

#define CAM_WIDTH   640
#define CAM_HEIGHT  480
#define FRAME_BYTES (640 * 480 * 2)

/* 双 PSRAM 缓冲：相机回调写入空闲缓冲，LVGL 读取就绪缓冲 */
static uint8_t *s_fb[2] = {NULL, NULL};
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

    /* 写入空闲缓冲 */
    int idx = (s_ready_idx == -1 || s_ready_idx == 0) ? 1 : 0;
    if (s_fb[idx] && stride * h <= FRAME_BYTES) {
        memcpy(s_fb[idx], buf, stride * h);
        size_t align_sz = (stride * h + 63) & ~63;
        esp_cache_msync(s_fb[idx], align_sz, ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        s_ready_idx = idx;
    }

    /* LVGL canvas 刷新 */
    if (lvgl_port_lock(0)) {
        int ridx = s_ready_idx;
        if (ridx >= 0 && s_fb[ridx]) {
            size_t align_sz = (stride * h + 63) & ~63;
            esp_cache_msync(s_fb[ridx], align_sz,
                            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            ui_update_camera_preview(s_fb[ridx], w, h, stride);
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
            .sw_rotate   = true,
        },
    };

    if (!bsp_display_start_with_config(&display_cfg)) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_backlight_on();

    /* 2. 分配双 PSRAM 帧缓冲 */
    for (int i = 0; i < 2; i++) {
        s_fb[i] = heap_caps_aligned_alloc(64, FRAME_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_fb[i]) {
            ESP_LOGE(TAG, "PSRAM fb[%d] alloc failed", i);
        }
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
