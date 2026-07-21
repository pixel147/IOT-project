#include "bsp/esp32_p4_function_ev_board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "ui.h"
#include "sdcard_init.h"
#include "pose_estimator.hpp"

#include <string.h>
#include <math.h>

static const char *TAG = "MAIN";

#define CAM_WIDTH   640
#define CAM_HEIGHT  480
#define FRAME_BYTES (640 * 480 * 2)

/* 乒乓缓冲 + 独立推理缓冲 + 独立显示缓冲 */
static uint8_t *s_fb[2] = {NULL, NULL};
static uint8_t *s_fb_infer = NULL;
static uint8_t *s_fb_display = NULL;            /* canvas 固定指向此处，永不切换 */
static volatile int s_ready_idx = -1;            /* 推理可读的缓冲索引，-1 = 无 */
static volatile int s_write_idx = 0;             /* 当前写入的乒乓索引 */
static uint32_t s_frame_w = 0, s_frame_h = 0, s_frame_stride = 0;

static bool s_demo_mode = false;                /* SD 卡 / 模型失败时使用模拟骨架 */

/* ---- 相机回调 — 写乒乓缓冲，LVGL 锁内同步到显示缓冲 ---- */
static void on_camera_frame(const uint8_t *buf, uint32_t len,
                            uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt)
{
    (void)len; (void)fmt;
    s_frame_w = w;
    s_frame_h = h;
    s_frame_stride = stride;

    /* 交替写入 s_fb[0]/[1]（给推理任务用），不碰显示缓冲 */
    int idx = s_write_idx;
    s_write_idx ^= 1;
    if (s_fb[idx] && stride * h <= FRAME_BYTES) {
        memcpy(s_fb[idx], buf, stride * h);
        size_t align_sz = (stride * h + 63) & ~63;
        esp_cache_msync(s_fb[idx], align_sz, ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        s_ready_idx = idx;
    }

    /* 锁内同步到显示缓冲——canvas 指针不动，只更新内容 */
    if (lvgl_port_lock(0)) {
        if (idx >= 0 && s_fb[idx] && s_fb_display) {
            memcpy(s_fb_display, s_fb[idx], stride * h);
            esp_cache_msync(s_fb_display, (stride * h + 63) & ~63,
                            ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
            ui_update_camera_display(s_fb_display, w, h, stride);
        }
        lvgl_port_unlock();
    }
}

/* ---- LVGL timer — 仅更新 FPS ---- */
static void fps_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_update_fps(cam_get_fps());
}

/* ================================================================
 * 几何辅助
 * ================================================================ */

/** 计算三点夹角（度），顶点为 p1 */
static float angle_between(const float p0[2], const float p1[2], const float p2[2])
{
    float dx1 = p0[0] - p1[0], dy1 = p0[1] - p1[1];
    float dx2 = p2[0] - p1[0], dy2 = p2[1] - p1[1];
    float dot = dx1 * dx2 + dy1 * dy2;
    float n1 = sqrtf(dx1 * dx1 + dy1 * dy1);
    float n2 = sqrtf(dx2 * dx2 + dy2 * dy2);
    if (n1 < 0.001f || n2 < 0.001f) return 0.0f;
    float cos_a = dot / (n1 * n2);
    if (cos_a > 1.0f) cos_a = 1.0f;
    if (cos_a < -1.0f) cos_a = -1.0f;
    return acosf(cos_a) * 180.0f / 3.14159265f;
}

/* ================================================================
 * Demo 模式：生成站立 COCO 17 骨架 + 手臂微摆
 * ================================================================ */
static void generate_demo_skeleton(float joints[17][2], float t)
{
    /* 标准站立姿势，[-1, 1] 归一化坐标 */
    static const float base[17][2] = {
        { 0.00f,  0.95f},  /* 0: 鼻子 */
        {-0.08f,  0.80f},  /* 1: 左眼 */
        { 0.08f,  0.80f},  /* 2: 右眼 */
        {-0.15f,  0.70f},  /* 3: 左耳 */
        { 0.15f,  0.70f},  /* 4: 右耳 */
        {-0.15f,  0.55f},  /* 5: 左肩 */
        { 0.15f,  0.55f},  /* 6: 右肩 */
        {-0.25f,  0.25f},  /* 7: 左肘 */
        { 0.25f,  0.25f},  /* 8: 右肘 */
        {-0.20f, -0.10f},  /* 9: 左腕 */
        { 0.20f, -0.10f},  /* 10: 右腕 */
        {-0.10f,  0.20f},  /* 11: 左髋 */
        { 0.10f,  0.20f},  /* 12: 右髋 */
        {-0.10f, -0.40f},  /* 13: 左膝 */
        { 0.10f, -0.40f},  /* 14: 右膝 */
        {-0.10f, -0.90f},  /* 15: 左踝 */
        { 0.10f, -0.90f},  /* 16: 右踝 */
    };

    memcpy(joints, base, sizeof(base));

    /* 手臂微摆动画 */
    float sway = sinf(t * 2.0f) * 0.08f;
    joints[7][0]  += sway;          /* 左肘 */
    joints[9][0]  += sway * 1.5f;   /* 左腕 */
    joints[8][0]  -= sway;          /* 右肘 */
    joints[10][0] -= sway * 1.5f;   /* 右腕 */
}

/* ================================================================
 * 推理任务 — Core 1, 优先 2（低于 LVGL/摄像头）
 * ================================================================ */
static void pose_inference_task(void *arg)
{
    (void)arg;

    /* 推理单次耗时 >10s，不注册 WDT（否则会误触发 panic） */
    /* 等待相机启动（最多 10 秒）或 demo 模式 */
    int wait = 0;
    while (!cam_is_running() && !s_demo_mode) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (++wait > 100) {
            ESP_LOGW(TAG, "Camera start timeout, entering demo mode");
            s_demo_mode = true;
            break;
        }
    }

    ESP_LOGI(TAG, "Inference task running, demo_mode=%d", s_demo_mode);

    float joints[17][2];
    float confidences[17];
    float detection_score;
    float demo_time = 0.0f;

    while (true) {
        /* ---- 每周期喂狗（超时设为 10s，避免长卷积触发报警） ---- */
        esp_task_wdt_reset();
        TickType_t cycle_start = xTaskGetTickCount();

        /* ---- frame 结构体置默认值 ---- */
        ui_pose_frame_t frame = { 0 };
        frame.has_angles = false;
        frame.has_score  = false;
        frame.guidance   = NULL;

        if (s_demo_mode) {
            /* ====================== Demo 模式 ====================== */
            demo_time += 0.333f;
            generate_demo_skeleton(joints, demo_time);
            memcpy(frame.joints, joints, sizeof(joints));
            memset(frame.deviations, 0, sizeof(frame.deviations));
            frame.has_score  = true;
            frame.score      = 85;
            frame.has_angles = true;
            float sway_angle = sinf(demo_time * 2.0f) * 10.0f;
            frame.left_knee  = 165.0f + sway_angle;
            frame.right_knee = 168.0f - sway_angle * 0.5f;
            frame.left_elbow  = 172.0f;
            frame.right_elbow = 170.0f;
            frame.guidance   = "提示：当前仅显示模拟骨架，模型未接入。";
        } else {
            /* ====================== 实时推理 ====================== */
            uint32_t fw = CAM_WIDTH, fh = CAM_HEIGHT;
            cam_get_frame_info(&fw, &fh);

            /* 从安全缓冲读取最新帧（避免相机回调覆盖竞争） */
            int idx = s_ready_idx;
            if (idx < 0 || !s_fb[idx] || !s_fb_infer) {
                frame.guidance = "提示：等待摄像头数据。";
                goto update_ui;
            }

            size_t copy_sz = s_frame_stride * s_frame_h;
            if (copy_sz > FRAME_BYTES) copy_sz = FRAME_BYTES;
            memcpy(s_fb_infer, s_fb[idx], copy_sz);

            /* CPU 写完 PSRAM 后 clean 缓存，让硬件加速器读到正确数据 */
            size_t align_sz = (copy_sz + 63) & ~63;
            esp_cache_msync(s_fb_infer, align_sz,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

            /* 执行推理 */
            int64_t t0 = esp_timer_get_time();
            esp_err_t ret = pose_estimator_run(
                s_fb_infer, fw, fh, s_frame_stride,
                joints, confidences, &detection_score);
            int64_t t1 = esp_timer_get_time();
            ESP_LOGI(TAG, "推理耗时: %lld ms", (t1 - t0) / 1000);

            if (ret == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Model unavailable, switching to demo mode");
                s_demo_mode = true;
                demo_time = 0.333f;
                generate_demo_skeleton(joints, demo_time);
                memcpy(frame.joints, joints, sizeof(joints));
                memset(frame.deviations, 0, sizeof(frame.deviations));
                frame.has_score  = true;
                frame.score      = 85;
                frame.has_angles = true;
                frame.left_knee  = 165.0f;
                frame.right_knee = 168.0f;
                frame.left_elbow  = 172.0f;
                frame.right_elbow = 170.0f;
                frame.guidance   = "提示：当前仅显示模拟骨架，模型未接入。";
            } else if (ret == ESP_FAIL) {
                frame.guidance = "提示：未检测到人体，请调整姿态。";
            } else {
                memcpy(frame.joints, joints, sizeof(joints));
                for (int i = 0; i < 17; i++) {
                    frame.deviations[i] = confidences[i] < 0.3f ? 2
                                         : confidences[i] < 0.6f ? 1 : 0;
                }
                float sum = 0.0f;
                for (int i = 0; i < 17; i++) sum += confidences[i];
                frame.has_score = true;
                frame.score = (uint8_t)(sum / 17.0f * 100.0f);
                frame.left_knee   = angle_between(joints[11], joints[13], joints[15]);
                frame.right_knee  = angle_between(joints[12], joints[14], joints[16]);
                frame.left_elbow  = angle_between(joints[5],  joints[7],  joints[9]);
                frame.right_elbow = angle_between(joints[6],  joints[8],  joints[10]);
                frame.has_angles = true;
                frame.guidance = "提示：已检测到人体。";
            }
        }

update_ui:
        /* ---- 更新 UI（需 LVGL 锁） ---- */
        if (lvgl_port_lock(0)) {
            ui_present_pose_frame(&frame);
            lvgl_port_unlock();
        }

        /* ---- 保证 333ms 周期，让出 CPU 给摄像头/LVGL ---- */
        TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        TickType_t min_cycle = pdMS_TO_TICKS(333);
        if (elapsed < min_cycle) {
            vTaskDelay(min_cycle - elapsed);
        }
    }
}

/* ================================================================
 * 入口
 * ================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Smart Vision Pose Assessment ===");

    /* ---- 1. 显示初始化 ---- */
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

    /* ---- 2. 分配 PSRAM 缓冲 ---- */
    for (int i = 0; i < 2; i++) {
        s_fb[i] = heap_caps_aligned_alloc(64, FRAME_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_fb[i]) {
            ESP_LOGE(TAG, "PSRAM buffer %d allocation failed", i);
            return;
        }
    }

    s_fb_infer = heap_caps_aligned_alloc(64, FRAME_BYTES,
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_fb_infer) {
        ESP_LOGE(TAG, "Inference buffer allocation failed");
        return;
    }

    s_fb_display = heap_caps_aligned_alloc(64, FRAME_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_fb_display) {
        ESP_LOGE(TAG, "Display buffer allocation failed");
        return;
    }
    memset(s_fb_display, 0, FRAME_BYTES);

    /* ---- 3. SD 卡挂载 + 模型加载 ---- */
#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t sd_ret = sdcard_init();
    if (sd_ret == ESP_OK) {
        pose_estimator_load_and_print();
    } else {
        s_demo_mode = true;
        ESP_LOGW(TAG, "SD card init failed (%s), entering demo mode",
                 esp_err_to_name(sd_ret));
    }
#else
    s_demo_mode = true;
    ESP_LOGW(TAG, "Non-P4 target, entering demo mode");
#endif

    /* ---- 4. UI 初始化 ---- */
    lvgl_port_lock(-1);
    ui_init();
    if (s_demo_mode) {
        ui_set_system_status("等待接入");
        ui_update_suggestion("提示：当前仅显示模拟骨架，模型未接入。");
    } else {
        ui_set_system_status("摄像头：启动中");
        ui_update_suggestion("提示：等待姿态模型接入。");
    }
    /* 设置 canvas 固定显示缓冲（永不切换指针） */
    ui_set_camera_buffer(s_fb_display, CAM_WIDTH, CAM_HEIGHT);
    lv_timer_create(fps_timer_cb, 1000, NULL);
    lvgl_port_unlock();

    /* ---- 5. 创建推理任务 (Core 1, 优先 2, 16KB 栈) ---- */
    /*   Core 1: 推理（低优先级，摄像头/LVGL 可抢占）               */
    /*   Core 0: LVGL + 摄像头处理，无推理干扰                       */
    if (xTaskCreatePinnedToCore(pose_inference_task, "pose_infer",
                                16384, NULL, 2, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Inference task creation failed");
        return;
    }

    /* ---- 6. 启动相机 ---- */
#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t cam_ret = cam_start(CAM_WIDTH, CAM_HEIGHT, 30, on_camera_frame);

    lvgl_port_lock(-1);
    if (cam_ret == ESP_OK) {
        ui_set_system_status(s_demo_mode ? "等待接入" : "摄像头：已连接");
    } else {
        ui_set_system_status("摄像头：失败");
        ESP_LOGE(TAG, "Camera start failed: %s", esp_err_to_name(cam_ret));
    }
    lvgl_port_unlock();
#endif

    ESP_LOGI(TAG, "System ready — camera + inference running");
}
