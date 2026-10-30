#include "nomi_ui.h"

#include <stdio.h>
#include <string.h>

#include "face.h"

#define FRAME_MS 33
#define STEP_S (1.0f / 60)
#define EMOTION_HOLD_MS 4000  // LLM 给的情绪保持多久
#define MIN_BRIGHTNESS 10     // 亮度滑条下限，免得调成全黑找不回来

#define C_TEXT 0xe8eaf0
#define C_MUTED 0x8b90a0
#define C_PANEL 0x1b1e26
#define C_ACCENT 0x6aa6ff

static nomi_ui_callbacks_t s_cb;
static const lv_font_t *s_font;
static uint16_t *s_buf;
static lv_obj_t *s_canvas, *s_status, *s_subtitle, *s_toast, *s_menu;
static lv_obj_t *s_volume_slider, *s_brightness_slider, *s_wifi_label;
static lv_timer_t *s_toast_timer;
static uint32_t s_last_ms, s_emotion_until;
static float s_acc;
static face_mode_t s_emotion_mode = FACE_IDLE;
static bool s_wifi_armed;  // 重新配网要点两次确认

/* ---------- 表情 ---------- */

static face_mode_t mode_for_emotion(const char *emotion)
{
    static const char *const happy[] = {"happy", "laughing", "funny",   "loving", "winking", "cool",
                                        "relaxed", "delicious", "kissy", "confident", "silly"};
    static const char *const confused[] = {"confused", "embarrassed", "surprised", "shocked"};
    for (size_t i = 0; i < sizeof(happy) / sizeof(happy[0]); i++) {
        if (strcmp(emotion, happy[i]) == 0) return FACE_HAPPY;
    }
    for (size_t i = 0; i < sizeof(confused) / sizeof(confused[0]); i++) {
        if (strcmp(emotion, confused[i]) == 0) return FACE_CONFUSED;
    }
    if (strcmp(emotion, "thinking") == 0) return FACE_THINK;
    return FACE_IDLE;
}

// 说话最优先，其次是 LLM 刚给的情绪，再看设备状态
static face_mode_t pick_mode(uint32_t now)
{
    nomi_state_t state = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
    if (state == NOMI_STATE_SPEAKING) return FACE_SPEAK;
    if (s_emotion_mode != FACE_IDLE && (int32_t)(s_emotion_until - now) > 0) return s_emotion_mode;
    switch (state) {
    case NOMI_STATE_LISTENING: return FACE_LISTEN;
    case NOMI_STATE_CONNECTING: return FACE_THINK;
    case NOMI_STATE_ERROR: return FACE_CONFUSED;
    default: return FACE_IDLE;
    }
}

static void face_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    float dt = (now - s_last_ms) / 1000.0f;
    s_last_ms = now;
    s_acc += dt > 0.1f ? 0.1f : dt;  // 卡顿后别一次追太多步

    face_mode_t mode = pick_mode(now);
    if (mode != face_get_mode()) face_set_mode(mode);
    for (; s_acc >= STEP_S; s_acc -= STEP_S) face_step(STEP_S);

    // 表情直接画进画布缓冲，只通知 LVGL 重画变化的那块
    rrect_t d = face_render(s_buf);
    if (!rrect_empty(d)) {
        lv_area_t area = {d.x0, d.y0, d.x1 - 1, d.y1 - 1};
        lv_obj_invalidate_area(s_canvas, &area);
    }
}

void nomi_ui_set_emotion(const char *emotion)
{
    if (!emotion) return;
    s_emotion_mode = mode_for_emotion(emotion);
    s_emotion_until = lv_tick_get() + EMOTION_HOLD_MS;
}

/* ---------- 文字 ---------- */

static void set_text_style(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

// 字体设在几个顶层对象上，菜单里的文字跟着继承
void nomi_ui_set_font(const lv_font_t *font)
{
    if (!font) return;
    s_font = font;
    lv_obj_t *roots[] = {s_status, s_subtitle, s_toast, s_menu};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        lv_obj_set_style_text_font(roots[i], font, 0);
    }
    lv_obj_set_height(s_subtitle, lv_font_get_line_height(font) * 2);  // 最多两行
}

void nomi_ui_set_status(const char *status)
{
    lv_label_set_text(s_status, status ? status : "");
}

void nomi_ui_set_subtitle(const char *role, const char *text)
{
    if (!text || !*text) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // 用户说的话偏灰，助手的回答用亮白
    bool from_user = role && strcmp(role, "user") == 0;
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(from_user ? C_MUTED : C_TEXT), 0);
    lv_label_set_text(s_subtitle, text);
    lv_obj_remove_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
}

static void hide_toast(lv_timer_t *timer)
{
    (void)timer;
    s_toast_timer = NULL;  // 只跑一次的定时器跑完会自己删掉
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

void nomi_ui_notify(const char *text, uint32_t duration_ms)
{
    if (!text || !*text) return;
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(hide_toast, duration_ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

/* ---------- 设置菜单 ---------- */

static void close_menu(void)
{
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    s_wifi_armed = false;
    lv_label_set_text(s_wifi_label, "重新配网");
}

static void open_menu(void)
{
    if (s_cb.get_volume) lv_slider_set_value(s_volume_slider, s_cb.get_volume(), LV_ANIM_OFF);
    if (s_cb.get_brightness) {
        int level = s_cb.get_brightness();
        lv_slider_set_value(s_brightness_slider, level < MIN_BRIGHTNESS ? MIN_BRIGHTNESS : level,
                            LV_ANIM_OFF);
    }
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_menu);
}

static void on_backdrop(lv_event_t *e)
{
    // 只有点在面板外面的遮罩上才关闭
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) close_menu();
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    bool final = lv_event_get_code(e) == LV_EVENT_RELEASED;
    if (slider == s_volume_slider && s_cb.set_volume) s_cb.set_volume(value, final);
    if (slider == s_brightness_slider && s_cb.set_brightness) s_cb.set_brightness(value, final);
}

enum { BTN_CONFIG_URL, BTN_WIFI, BTN_ABOUT, BTN_CLOSE };

static void on_menu_button(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case BTN_CONFIG_URL: {
        const char *url = s_cb.get_config_url ? s_cb.get_config_url() : NULL;
        char text[128];
        if (url && *url) {
            snprintf(text, sizeof(text), "用浏览器打开 %s", url);
        } else {
            snprintf(text, sizeof(text), "还没连上网络");
        }
        close_menu();
        nomi_ui_notify(text, 8000);
        break;
    }
    case BTN_WIFI:
        if (!s_wifi_armed) {
            s_wifi_armed = true;
            lv_label_set_text(s_wifi_label, "再点一次确认");
            break;
        }
        close_menu();
        if (s_cb.reconfigure_wifi) s_cb.reconfigure_wifi();
        break;
    case BTN_ABOUT:
        close_menu();
        nomi_ui_notify(s_cb.get_about ? s_cb.get_about() : "Pocket Companion", 6000);
        break;
    default:
        close_menu();
        break;
    }
}

static lv_obj_t *add_slider_row(lv_obj_t *parent, const char *title, int min)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);
    set_text_style(label, C_MUTED);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, min, 100);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2c3040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_RELEASED, NULL);
    return slider;
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, intptr_t id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c3040), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_add_event_cb(btn, on_menu_button, LV_EVENT_CLICKED, (void *)id);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    set_text_style(label, C_TEXT);
    return label;
}

static void build_menu(lv_obj_t *screen)
{
    s_menu = lv_obj_create(screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_menu, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_60, 0);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_menu, on_backdrop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_menu);
    lv_obj_set_size(panel, 340, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "设置");
    set_text_style(title, C_TEXT);

    s_volume_slider = add_slider_row(panel, "音量", 0);
    s_brightness_slider = add_slider_row(panel, "亮度", MIN_BRIGHTNESS);

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(buttons, 8, 0);
    lv_obj_set_style_pad_top(buttons, 8, 0);
    add_button(buttons, "设置页地址", BTN_CONFIG_URL);
    s_wifi_label = add_button(buttons, "重新配网", BTN_WIFI);
    add_button(buttons, "关于", BTN_ABOUT);
    add_button(buttons, "关闭", BTN_CLOSE);

    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- 入口 ---------- */

static void on_face(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_cb.toggle_chat) s_cb.toggle_chat();
    } else if (code == LV_EVENT_LONG_PRESSED) {
        open_menu();
    }
}

void nomi_ui_create(lv_obj_t *screen, uint16_t *face_buf, const lv_font_t *font,
                    const nomi_ui_callbacks_t *callbacks)
{
    s_buf = face_buf;
    s_font = font;
    s_cb = *callbacks;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    face_init(lv_tick_get() * 2654435761u + 1);
    face_render(s_buf);  // 第一帧整屏画好
    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(s_canvas, s_buf, FACE_W, FACE_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_LONG_PRESSED, NULL);

    s_status = lv_label_create(screen);
    set_text_style(s_status, C_MUTED);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(s_status, "");

    s_subtitle = lv_label_create(screen);
    set_text_style(s_subtitle, C_TEXT);
    lv_obj_set_width(s_subtitle, FACE_W - 40);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);

    s_toast = lv_label_create(screen);
    set_text_style(s_toast, C_TEXT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_set_style_max_width(s_toast, FACE_W - 40, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    build_menu(screen);
    nomi_ui_set_font(font);

    s_last_ms = lv_tick_get();
    lv_timer_create(face_tick, FRAME_MS, NULL);
}
