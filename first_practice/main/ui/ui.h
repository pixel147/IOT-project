#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_POSE_JOINT_COUNT 17
/*
 * 未来姿态任务与 UI 任务之间的约定。仅在持有
 * bsp_display_lock() 时调用 ui_present_pose_frame()。
 */
typedef struct {
    float joints[UI_POSE_JOINT_COUNT][2];
    uint8_t deviations[UI_POSE_JOINT_COUNT];
    float left_knee;
    float right_knee;
    float left_elbow;
    float right_elbow;
    uint8_t score;
    bool has_angles;
    bool has_score;
    const char *guidance;
} ui_pose_frame_t;

void ui_create_main_screen(void);
void ui_init(void);

lv_obj_t *ui_get_skeleton_container(void);
lv_obj_t *ui_get_camera_container(void);

void ui_update_skeleton(const float joint_positions[][2], int num_joints,
                        const uint8_t *deviations);
void ui_update_angles(float left_knee, float right_knee, float left_elbow,
                      float right_elbow);
void ui_update_score(uint8_t score);
void ui_update_suggestion(const char *text);
void ui_set_mode_text(const char *mode);
void ui_update_camera_preview(const uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t stride);
void ui_update_fps(float fps);
void ui_set_system_status(const char *status);
void ui_present_pose_frame(const ui_pose_frame_t *frame);
void ui_show_message(const char *title, const char *msg, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif
