#include "nomi_display.h"

#include "application.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>

#include <cstring>

#define TAG "NomiDisplay"

namespace {

constexpr uint32_t kFrameMs = 33;
constexpr float kStepS = 1.0f / 60;
constexpr uint32_t kEmotionHoldMs = 4000;  // LLM 给的情绪保持多久

face_mode_t ModeForEmotion(const char* emotion) {
    static const char* const kHappy[] = {"happy",   "laughing", "funny", "loving",
                                         "winking", "cool",     "relaxed", "delicious",
                                         "kissy",   "confident", "silly"};
    static const char* const kConfused[] = {"confused", "embarrassed", "surprised", "shocked"};
    for (auto e : kHappy) {
        if (strcmp(emotion, e) == 0) return FACE_HAPPY;
    }
    for (auto e : kConfused) {
        if (strcmp(emotion, e) == 0) return FACE_CONFUSED;
    }
    if (strcmp(emotion, "thinking") == 0) return FACE_THINK;
    return FACE_IDLE;
}

}  // namespace

NomiDisplay::NomiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y, bool mirror_x,
                         bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void NomiDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    SpiLcdDisplay::SetupUI();

    // 黑脸配深色主题，状态栏和字幕才是浅色字
    auto* dark = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark != nullptr) {
        SetTheme(dark);
    }

    DisplayLockGuard lock(this);
    face_buf_ = static_cast<uint16_t*>(
        heap_caps_malloc(FACE_W * FACE_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (face_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate face buffer");
        return;
    }
    face_init(esp_random());
    face_render(face_buf_);

    canvas_ = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(canvas_, face_buf_, FACE_W, FACE_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas_, 0, 0);
    lv_obj_move_background(canvas_);
    MakeChromeTransparent();

    last_ms_ = lv_tick_get();
    lv_timer_create(OnTick, kFrameMs, this);
}

void NomiDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    MakeChromeTransparent();
}

// 小智自带界面各层都有底色，去掉后脸才露得出来；表情图标位也不用了
void NomiDisplay::MakeChromeTransparent() {
    for (lv_obj_t* obj : {container_, content_, top_bar_, status_bar_, side_bar_, bottom_bar_}) {
        if (obj != nullptr) {
            lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(obj, 0, 0);
        }
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
}

void NomiDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    emotion_mode_ = ModeForEmotion(emotion);
    emotion_until_ = lv_tick_get() + kEmotionHoldMs;
}

// 说话最优先，其次是 LLM 刚给的情绪，再看是不是在听
face_mode_t NomiDisplay::PickMode(uint32_t now) const {
    auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        return FACE_SPEAK;
    }
    if (emotion_mode_ != FACE_IDLE && static_cast<int32_t>(emotion_until_ - now) > 0) {
        return emotion_mode_;
    }
    switch (state) {
        case kDeviceStateListening:
            return FACE_LISTEN;
        case kDeviceStateConnecting:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading:
            return FACE_THINK;
        case kDeviceStateFatalError:
            return FACE_CONFUSED;
        default:
            return FACE_IDLE;
    }
}

void NomiDisplay::OnTick(lv_timer_t* timer) {
    static_cast<NomiDisplay*>(lv_timer_get_user_data(timer))->Tick();
}

void NomiDisplay::Tick() {
    uint32_t now = lv_tick_get();
    float dt = (now - last_ms_) / 1000.0f;
    last_ms_ = now;
    acc_ += dt > 0.1f ? 0.1f : dt;  // 卡顿后别一次追太多步

    face_mode_t mode = PickMode(now);
    if (mode != face_get_mode()) {
        face_set_mode(mode);
    }
    for (; acc_ >= kStepS; acc_ -= kStepS) {
        face_step(kStepS);
    }

    // 表情直接画进画布缓冲，只通知 LVGL 重画变化的那块
    rrect_t d = face_render(face_buf_);
    if (!rrect_empty(d)) {
        lv_area_t area = {d.x0, d.y0, d.x1 - 1, d.y1 - 1};
        lv_obj_invalidate_area(canvas_, &area);
    }
}
