#include "ui.h"
#include "ui_font_zh_22.h"

#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_14);

#define UI_JOINT_COUNT 17
#define UI_JOINT_PAIR_COUNT 12

static lv_obj_t *skeleton_container;
static lv_obj_t *camera_container;
static lv_obj_t *camera_img;
static lv_obj_t *angle_label;
static lv_obj_t *score_label;
static lv_obj_t *fps_label;
static lv_obj_t *status_label;
static lv_obj_t *mode_label_statusbar;
static lv_obj_t *suggestion_panel;
static lv_obj_t *suggestion_label;
static lv_obj_t *start_stop_label;
static lv_obj_t *message_popup;
static lv_timer_t *message_timer;

static uint32_t preview_width = 4;
static uint32_t preview_height = 3;

static lv_point_precise_t skeleton_lines[UI_JOINT_PAIR_COUNT][2];

static const uint8_t joint_pairs[UI_JOINT_PAIR_COUNT][2] = {
    {5, 6}, {5, 7}, {7, 9}, {6, 8}, {8, 10}, {5, 11},
    {6, 12}, {11, 12}, {11, 13}, {13, 15}, {12, 14}, {14, 16},
};

static void set_ui_text_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &ui_font_zh_22, 0);
}

static void set_symbol_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
}

static void set_panel_style(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
}

static void norm_to_pixel(float nx, float ny, float *px, float *py)
{
    const float content_w = (float)lv_obj_get_content_width(skeleton_container);
    const float content_h = (float)lv_obj_get_content_height(skeleton_container);
    const float scale_x = content_w / (float)preview_width;
    const float scale_y = content_h / (float)preview_height;
    const float scale = scale_x < scale_y ? scale_x : scale_y;
    const float image_w = (float)preview_width * scale;
    const float image_h = (float)preview_height * scale;
    const float image_x = (content_w - image_w) * 0.5f;
    const float image_y = (content_h - image_h) * 0.5f;

    *px = image_x + (nx + 1.0f) * 0.5f * image_w;
    *py = image_y + (ny + 1.0f) * 0.5f * image_h;
}

static void reset_measurements(void)
{
    if (score_label) {
        lv_label_set_text(score_label, "评分：--");
        lv_obj_set_style_text_color(score_label, lv_color_hex(0x8B949E), 0);
    }
    if (angle_label) {
        lv_label_set_text(angle_label,
                          "左膝：--\n右膝：--\n左肘：--\n右肘：--");
    }
}

static void mode_btn_event_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_obj_get_child(lv_event_get_target_obj(e), 0);
    if (label) {
        ui_set_mode_text(lv_label_get_text(label));
    }
}

static void start_stop_event_cb(lv_event_t *e)
{
    static bool observing;

    (void)e;
    observing = !observing;
    lv_label_set_text(start_stop_label, observing ? "停止观察" : "开始观察");
}

static void hide_message_cb(lv_timer_t *timer)
{
    if (message_popup) {
        lv_obj_add_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
    }
    message_timer = NULL;
    lv_timer_del(timer);
}

lv_obj_t *ui_get_skeleton_container(void)
{
    return skeleton_container;
}

lv_obj_t *ui_get_camera_container(void)
{
    return camera_container;
}

void ui_create_main_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E27), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, LV_PCT(100), 46);
    set_panel_style(status_bar, lv_color_hex(0x18213D));
    lv_obj_set_style_pad_hor(status_bar, 16, 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *time_label = lv_label_create(status_bar);
    lv_label_set_text(time_label, "12:36");
    set_symbol_font(time_label);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);

    mode_label_statusbar = lv_label_create(status_bar);
    lv_label_set_text(mode_label_statusbar, "俯卧撑观察");
    set_ui_text_font(mode_label_statusbar);
    lv_obj_set_style_text_color(mode_label_statusbar, lv_color_hex(0x4FC3F7), 0);

    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "摄像头：启动中");
    set_ui_text_font(status_label);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8B949E), 0);

    fps_label = lv_label_create(status_bar);
    lv_label_set_text(fps_label, "0 FPS");
    set_symbol_font(fps_label);
    lv_obj_set_style_text_color(fps_label, lv_color_hex(0x8B949E), 0);

    lv_obj_t *icons = lv_label_create(status_bar);
    lv_label_set_text(icons, LV_SYMBOL_WIFI "  " LV_SYMBOL_BATTERY_FULL);
    set_symbol_font(icons);
    lv_obj_set_style_text_color(icons, lv_color_white(), 0);

    lv_obj_t *middle_row = lv_obj_create(scr);
    lv_obj_set_size(middle_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(middle_row, 1);
    lv_obj_set_flex_flow(middle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(middle_row, 0, 0);
    lv_obj_set_style_border_width(middle_row, 0, 0);
    lv_obj_set_style_bg_opa(middle_row, LV_OPA_TRANSP, 0);

    lv_obj_t *menu_panel = lv_obj_create(middle_row);
    lv_obj_set_size(menu_panel, 132, LV_PCT(100));
    set_panel_style(menu_panel, lv_color_hex(0x18213D));
    lv_obj_set_style_pad_all(menu_panel, 12, 0);
    lv_obj_set_style_pad_row(menu_panel, 12, 0);
    lv_obj_set_flex_flow(menu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    const char *const menu_names[] = {"俯卧撑", "引体向上", "仰卧起坐"};
    for (uint32_t i = 0; i < sizeof(menu_names) / sizeof(menu_names[0]); i++) {
        lv_obj_t *btn = lv_btn_create(menu_panel);
        lv_obj_set_size(btn, LV_PCT(100), 72);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, menu_names[i]);
        set_ui_text_font(label);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, mode_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *pose_stage = lv_obj_create(middle_row);
    lv_obj_set_flex_grow(pose_stage, 1);
    lv_obj_set_height(pose_stage, LV_PCT(100));
    set_panel_style(pose_stage, lv_color_hex(0x101428));
    lv_obj_set_style_pad_all(pose_stage, 0, 0);
    lv_obj_set_scrollbar_mode(pose_stage, LV_SCROLLBAR_MODE_OFF);

    camera_container = lv_obj_create(pose_stage);
    lv_obj_set_size(camera_container, LV_PCT(100), LV_PCT(100));
    lv_obj_center(camera_container);
    lv_obj_set_style_bg_color(camera_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(camera_container, LV_OPA_TRANSP, 0);  /* 背景透明，相机像素由 app 直接写 draw buffer */
    lv_obj_set_style_border_width(camera_container, 0, 0);
    lv_obj_set_style_pad_all(camera_container, 0, 0);
    lv_obj_set_scrollbar_mode(camera_container, LV_SCROLLBAR_MODE_OFF);

    /* 使用 lv_canvas 而非 lv_image — 直接当位图显示，跳过图像解码器 */
    camera_img = lv_canvas_create(camera_container);
    lv_obj_set_size(camera_img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(camera_img);
    /* 缓冲由 app_main 分配并设置（不在此处设置，canvas 暂时无缓冲） */

    skeleton_container = lv_obj_create(pose_stage);
    lv_obj_set_size(skeleton_container, LV_PCT(100), LV_PCT(100));
    lv_obj_center(skeleton_container);
    lv_obj_set_style_bg_opa(skeleton_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(skeleton_container, 0, 0);
    lv_obj_set_style_pad_all(skeleton_container, 0, 0);
    lv_obj_set_scrollbar_mode(skeleton_container, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *angle_panel = lv_obj_create(middle_row);
    lv_obj_set_size(angle_panel, 174, LV_PCT(100));
    set_panel_style(angle_panel, lv_color_hex(0x18213D));
    lv_obj_set_style_pad_all(angle_panel, 16, 0);
    lv_obj_set_style_pad_row(angle_panel, 12, 0);
    lv_obj_set_flex_flow(angle_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(angle_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *metrics_title = lv_label_create(angle_panel);
    lv_label_set_text(metrics_title, "实时数据");
    set_ui_text_font(metrics_title);
    lv_obj_set_style_text_color(metrics_title, lv_color_hex(0x4FC3F7), 0);

    score_label = lv_label_create(angle_panel);
    set_ui_text_font(score_label);
    lv_obj_t *sep = lv_obj_create(angle_panel);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);

    angle_label = lv_label_create(angle_panel);
    set_ui_text_font(angle_label);
    lv_obj_set_style_text_color(angle_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(angle_label, 8, 0);

    lv_obj_t *model_state = lv_label_create(angle_panel);
    lv_label_set_text(model_state, "等待姿态模型接入");
    set_ui_text_font(model_state);
    lv_obj_set_style_text_color(model_state, lv_color_hex(0x8B949E), 0);
    lv_label_set_long_mode(model_state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(model_state, LV_PCT(100));
    reset_measurements();

    suggestion_panel = lv_obj_create(scr);
    lv_obj_set_size(suggestion_panel, LV_PCT(100), LV_SIZE_CONTENT);
    set_panel_style(suggestion_panel, lv_color_hex(0x101428));
    lv_obj_set_style_pad_all(suggestion_panel, 12, 0);
    lv_obj_set_style_border_side(suggestion_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(suggestion_panel, 1, 0);
    lv_obj_set_style_border_color(suggestion_panel, lv_color_hex(0x30363D), 0);
    suggestion_label = lv_label_create(suggestion_panel);
    lv_obj_set_width(suggestion_label, LV_PCT(100));
    lv_label_set_long_mode(suggestion_label, LV_LABEL_LONG_WRAP);
    set_ui_text_font(suggestion_label);
    lv_obj_set_style_text_color(suggestion_label, lv_color_hex(0xFFC107), 0);
    lv_label_set_text(suggestion_label, "提示：等待姿态模型接入，当前仅显示模拟骨架。");

    lv_obj_t *control_bar = lv_obj_create(scr);
    lv_obj_set_size(control_bar, LV_PCT(100), 56);
    set_panel_style(control_bar, lv_color_hex(0x18213D));
    lv_obj_set_flex_flow(control_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(control_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(control_bar, 18, 0);

    lv_obj_t *start_stop_btn = lv_btn_create(control_bar);
    lv_obj_set_size(start_stop_btn, 140, 40);
    start_stop_label = lv_label_create(start_stop_btn);
    lv_label_set_text(start_stop_label, "开始观察");
    set_ui_text_font(start_stop_label);
    lv_obj_center(start_stop_label);
    lv_obj_add_event_cb(start_stop_btn, start_stop_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *settings_btn = lv_btn_create(control_bar);
    lv_obj_set_size(settings_btn, 56, 40);
    lv_obj_t *settings_icon = lv_label_create(settings_btn);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    set_symbol_font(settings_icon);
    lv_obj_center(settings_icon);

    message_popup = lv_obj_create(scr);
    lv_obj_set_size(message_popup, LV_PCT(58), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(message_popup, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_border_width(message_popup, 1, 0);
    lv_obj_set_style_border_color(message_popup, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_radius(message_popup, 6, 0);
    lv_obj_set_style_pad_all(message_popup, 16, 0);
    lv_obj_set_flex_flow(message_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(message_popup, 8, 0);
    lv_obj_center(message_popup);
    lv_obj_add_flag(message_popup, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_skeleton(const float joint_positions[][2], int num_joints,
                        const uint8_t *deviations)
{
    if (!skeleton_container) return;

    lv_obj_clean(skeleton_container);
    if (!joint_positions || num_joints < UI_JOINT_COUNT) return;

    lv_point_precise_t points[UI_JOINT_COUNT];
    for (uint32_t i = 0; i < UI_JOINT_COUNT; i++) {
        norm_to_pixel(joint_positions[i][0], joint_positions[i][1],
                      &points[i].x, &points[i].y);
    }

    for (uint32_t i = 0; i < UI_JOINT_PAIR_COUNT; i++) {
        const uint8_t first = joint_pairs[i][0];
        const uint8_t second = joint_pairs[i][1];
        uint8_t deviation = deviations ? (deviations[first] > deviations[second]
                                             ? deviations[first] : deviations[second])
                                      : 0;
        lv_color_t color = deviation == 0 ? lv_color_hex(0x00E676)
                         : deviation == 1 ? lv_color_hex(0xFFC107)
                                          : lv_color_hex(0xFF5252);

        skeleton_lines[i][0] = points[first];
        skeleton_lines[i][1] = points[second];
        lv_obj_t *line = lv_line_create(skeleton_container);
        lv_line_set_points(line, skeleton_lines[i], 2);
        lv_obj_set_style_line_color(line, color, 0);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }

    for (uint32_t i = 0; i < UI_JOINT_COUNT; i++) {
        lv_obj_t *joint = lv_obj_create(skeleton_container);
        lv_obj_set_size(joint, 8, 8);
        lv_obj_set_pos(joint, (lv_coord_t)(points[i].x - 4),
                       (lv_coord_t)(points[i].y - 4));
        lv_obj_set_style_bg_color(joint, lv_color_hex(0xE8F5E9), 0);
        lv_obj_set_style_border_width(joint, 0, 0);
        lv_obj_set_style_radius(joint, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(joint, 0, 0);
    }
}

void ui_update_angles(float left_knee, float right_knee, float left_elbow,
                      float right_elbow)
{
    if (!angle_label) return;
    lv_label_set_text_fmt(angle_label,
                          "左膝：%.1f°\n右膝：%.1f°\n左肘：%.1f°\n右肘：%.1f°",
                          left_knee, right_knee, left_elbow, right_elbow);
}

void ui_update_score(uint8_t score)
{
    if (!score_label) return;
    lv_label_set_text_fmt(score_label, "评分：%u", (unsigned)score);
    lv_obj_set_style_text_color(score_label,
                                score >= 80 ? lv_color_hex(0x3FB950)
                                : score >= 60 ? lv_color_hex(0xD29922)
                                              : lv_color_hex(0xF85149), 0);
}

void ui_update_suggestion(const char *text)
{
    if (!suggestion_panel || !suggestion_label) return;
    if (!text || text[0] == '\0') {
        lv_label_set_text(suggestion_label, "提示：等待姿态模型接入，当前仅显示模拟骨架。");
        return;
    }
    lv_label_set_text(suggestion_label, text);
    lv_obj_clear_flag(suggestion_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_mode_text(const char *mode)
{
    if (mode_label_statusbar && mode) {
        lv_label_set_text(mode_label_statusbar, mode);
    }
}

void ui_set_camera_buffer(const uint8_t *buf, uint32_t w, uint32_t h)
{
    if (!camera_img || !buf) return;
    lv_canvas_set_buffer(camera_img, (void *)buf, (lv_coord_t)w, (lv_coord_t)h,
                         LV_COLOR_FORMAT_RGB565);
}

void ui_update_camera_preview(const uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t stride)
{
    if (!camera_img || !buf || w == 0 || h == 0 || stride < w * 2) return;

    preview_width = w;
    preview_height = h;
    /* lv_canvas_set_buf: 直接指向相机缓冲，零拷贝，不解码 */
    lv_canvas_set_buffer(camera_img, (void *)buf, (lv_coord_t)w, (lv_coord_t)h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(camera_img);
}

/** 更新相机显示缓冲内容（canvas 指针固定于显示缓冲，仅刷新内容）。 */
void ui_update_camera_display(const uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t stride)
{
    if (!camera_img || !buf || w == 0 || h == 0 || stride < w * 2) return;
    preview_width = w;
    preview_height = h;
    lv_obj_invalidate(camera_img);
}

void ui_update_fps(float fps)
{
    if (fps_label) {
        lv_label_set_text_fmt(fps_label, "%.0f FPS", (double)fps);
    }
}

void ui_get_preview_rect(int *x, int *y, int *w, int *h)
{
    if (!camera_img) {
        *x = 0; *y = 0; *w = 320; *h = 240;
        return;
    }
    *x = lv_obj_get_x(camera_img);
    *y = lv_obj_get_y(camera_img);
    *w = lv_obj_get_width(camera_img);
    *h = lv_obj_get_height(camera_img);
}

void ui_set_system_status(const char *status)
{
    if (status_label && status) {
        lv_label_set_text(status_label, status);
    }
}

void ui_present_pose_frame(const ui_pose_frame_t *frame)
{
    if (!frame) return;

    ui_update_skeleton(frame->joints, UI_JOINT_COUNT, frame->deviations);
    if (frame->has_angles) {
        ui_update_angles(frame->left_knee, frame->right_knee,
                         frame->left_elbow, frame->right_elbow);
    } else {
        reset_measurements();
    }
    if (frame->has_score) {
        ui_update_score(frame->score);
    }
    ui_update_suggestion(frame->guidance);
}

void ui_show_message(const char *title, const char *msg, uint32_t duration_ms)
{
    if (!message_popup) return;
    if (message_timer) {
        lv_timer_del(message_timer);
        message_timer = NULL;
    }

    lv_obj_clean(message_popup);
    lv_obj_t *title_label = lv_label_create(message_popup);
    lv_label_set_text(title_label, title ? title : "提示");
    set_ui_text_font(title_label);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);

    lv_obj_t *message_label = lv_label_create(message_popup);
    lv_label_set_text(message_label, msg ? msg : "");
    set_ui_text_font(message_label);
    lv_obj_set_style_text_color(message_label, lv_color_hex(0xC9D1D9), 0);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message_label, LV_PCT(100));
    lv_obj_clear_flag(message_popup, LV_OBJ_FLAG_HIDDEN);

    if (duration_ms > 0) {
        message_timer = lv_timer_create(hide_message_cb, duration_ms, NULL);
    }
}

void ui_init(void)
{
    ui_create_main_screen();
}
