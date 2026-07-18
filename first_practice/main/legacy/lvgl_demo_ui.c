/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <math.h>
#include "lvgl.h"

#ifndef PI
#define PI  (3.14159f)
#endif

// LVGL 图像声明
LV_IMG_DECLARE(esp_logo)
LV_IMG_DECLARE(esp_text)

typedef struct {
    lv_obj_t *scr;
    int count_val;
} my_timer_context_t;

static lv_obj_t *arc[3];
static lv_obj_t *img_logo;
static lv_obj_t *img_text;
static lv_color_t arc_color[] = {
    LV_COLOR_MAKE(232, 87, 116),
    LV_COLOR_MAKE(126, 87, 162),
    LV_COLOR_MAKE(90, 202, 228),
};

#if (CONFIG_ESP_LCD_TOUCH_MAX_POINTS > 1 && CONFIG_LV_USE_GESTURE_RECOGNITION)
void gesture_cb(lv_event_t *e)
{
    static uint32_t logo_scale = LV_SCALE_NONE;
    static int32_t logo_rotation = 0;
    static int32_t logo_offset_x = 0;
    static int32_t logo_offset_y = 0;

    lv_indev_gesture_type_t type = lv_event_get_gesture_type(e);
    lv_indev_gesture_state_t state = lv_event_get_gesture_state(e, type);

    if (state == LV_INDEV_GESTURE_STATE_RECOGNIZED || state == LV_INDEV_GESTURE_STATE_ENDED) {
        switch (type) {
        case LV_INDEV_GESTURE_PINCH:
            // 读取当前缩放手势比例并从浮点数转换为图像缩放值
            uint32_t pinch_scale_rel = lv_event_get_pinch_scale(e) * LV_SCALE_NONE;
            // 将负值截断为 0
            uint32_t pinch_scale_abs = logo_scale + pinch_scale_rel < LV_SCALE_NONE ? 0 : logo_scale + ((
                                           int32_t)pinch_scale_rel - LV_SCALE_NONE);
            lv_img_set_zoom(img_logo, pinch_scale_abs);
            // 手势结束后保存最终的图像缩放值
            if (state == LV_INDEV_GESTURE_STATE_ENDED) {
                logo_scale = pinch_scale_abs;
            }
            break;
        case LV_INDEV_GESTURE_ROTATE:
            // 获取手势旋转角度（度）
            int32_t rotation_rel = lv_event_get_rotation(e) * (180.0f / M_PI) * 10.0f;
            // 计算结果角度并将其限制在 360 度内
            int32_t rotation_abs = (logo_rotation + rotation_rel) % 3600;
            lv_img_set_angle(img_logo, rotation_abs);
            // 手势结束后保存最终的图像旋转角度
            if (state == LV_INDEV_GESTURE_STATE_ENDED) {
                logo_rotation = rotation_abs;
            }
            break;
        case LV_INDEV_GESTURE_TWO_FINGERS_SWIPE:
            // 获取双指滑动值
            lv_dir_t swipe_dir = lv_event_get_two_fingers_swipe_dir(e);
            float swipe_dist = lv_event_get_two_fingers_swipe_distance(e);
            int32_t swipe_dist_abs;
            switch (swipe_dir) {
            case LV_DIR_BOTTOM:
            case LV_DIR_TOP:
                // 垂直偏移符号取决于滑动方向
                swipe_dist_abs = swipe_dir == LV_DIR_TOP ?
                                 logo_offset_y - swipe_dist :
                                 logo_offset_y + swipe_dist;
                lv_obj_set_pos(img_logo, logo_offset_x, swipe_dist_abs);
                // 手势结束后保存最终的垂直图像偏移
                if (state == LV_INDEV_GESTURE_STATE_ENDED) {
                    logo_offset_y = swipe_dist_abs;
                }
                break;
            case LV_DIR_RIGHT:
            case LV_DIR_LEFT:
                // 水平偏移符号取决于滑动方向
                swipe_dist_abs = swipe_dir == LV_DIR_LEFT ?
                                 logo_offset_x - swipe_dist :
                                 logo_offset_x + swipe_dist;
                lv_obj_set_pos(img_logo, swipe_dist_abs, logo_offset_y);
                // 手势结束后保存最终的水平图像偏移
                if (state == LV_INDEV_GESTURE_STATE_ENDED) {
                    logo_offset_x = swipe_dist_abs;
                }
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}
#endif

static void anim_timer_cb(lv_timer_t *timer)
{
    my_timer_context_t *timer_ctx = (my_timer_context_t *) lv_timer_get_user_data(timer);
    int count = timer_ctx->count_val;
    lv_obj_t *scr = timer_ctx->scr;

    // 播放圆弧动画
    if (count < 90) {
        lv_coord_t arc_start = count > 0 ? (1 - cosf(count / 180.0f * PI)) * 270 : 0;
        lv_coord_t arc_len = (sinf(count / 180.0f * PI) + 1) * 135;

        for (size_t i = 0; i < sizeof(arc) / sizeof(arc[0]); i++) {
            lv_arc_set_bg_angles(arc[i], arc_start, arc_len);
            lv_arc_set_rotation(arc[i], (count + 120 * (i + 1)) % 360);
        }
    }

    // 动画完成时删除圆弧
    if (count == 90) {
        for (size_t i = 0; i < sizeof(arc) / sizeof(arc[0]); i++) {
            lv_obj_del(arc[i]);
        }

        // 创建新图像并设为透明
        img_text = lv_img_create(scr);
        lv_img_set_src(img_text, &esp_text);
        lv_obj_set_style_img_opa(img_text, 0, 0);
    }

    // 圆弧动画完成时移动图像
    if ((count >= 100) && (count <= 180)) {
        lv_coord_t offset = (sinf((count - 140) * 2.25f / 90.0f) + 1) * 20.0f;
        lv_obj_align(img_logo, LV_ALIGN_CENTER, 0, -offset);
        lv_obj_align(img_text, LV_ALIGN_CENTER, 0, 2 * offset);
        lv_obj_set_style_img_opa(img_text, offset / 40.0f * 255, 0);
    }

    // 所有动画完成时删除定时器
    if ((count += 5) == 220) {
        lv_timer_del(timer);
    } else {
        timer_ctx->count_val = count;
    }
}

void example_lvgl_demo_ui(lv_obj_t *scr)
{
    // 创建图像
    img_logo = lv_img_create(scr);
    lv_img_set_src(img_logo, &esp_logo);
    lv_obj_center(img_logo);

#if (CONFIG_ESP_LCD_TOUCH_MAX_POINTS > 1 && CONFIG_LV_USE_GESTURE_RECOGNITION)
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
#endif

    // 创建圆弧
    for (size_t i = 0; i < sizeof(arc) / sizeof(arc[0]); i++) {
        arc[i] = lv_arc_create(scr);

        // 设置圆弧标题
        lv_obj_set_size(arc[i], 220 - 30 * i, 220 - 30 * i);
        lv_arc_set_bg_angles(arc[i], 120 * i, 10 + 120 * i);
        lv_arc_set_value(arc[i], 0);

        // 设置圆弧样式
        lv_obj_remove_style(arc[i], NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_width(arc[i], 10, 0);
        lv_obj_set_style_arc_color(arc[i], arc_color[i], 0);

        // 使圆弧居中
        lv_obj_center(arc[i]);
    }

    // 创建动画定时器
    static my_timer_context_t my_tim_ctx = {
        .count_val = -90,
    };
    my_tim_ctx.scr = scr;
    lv_timer_create(anim_timer_cb, 20, &my_tim_ctx);
}
