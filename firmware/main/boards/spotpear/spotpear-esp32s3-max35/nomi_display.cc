#include "nomi_display.h"

#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "nomi_ui.h"
#include "settings.h"
#include "wifi_board.h"
#if CONFIG_DIRECT_API
#include "direct_api_config_server.h"
#endif

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#define TAG "NomiDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

// 以下回调在 LVGL 任务里被 nomi_ui 调用

nomi_state_t GetState() {
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateListening: return NOMI_STATE_LISTENING;
        case kDeviceStateSpeaking: return NOMI_STATE_SPEAKING;
        case kDeviceStateConnecting:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading: return NOMI_STATE_CONNECTING;
        case kDeviceStateFatalError: return NOMI_STATE_ERROR;
        default: return NOMI_STATE_IDLE;
    }
}

void ToggleChat() { Application::GetInstance().ToggleChatState(); }

void SetVolume(int volume, bool final) {
    // SetOutputVolume 每次都写 NVS，拖动中不调，松手再设
    if (final) Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume);
}

int GetVolume() { return Board::GetInstance().GetAudioCodec()->output_volume(); }

void SetBrightness(int level, bool final) {
    Board::GetInstance().GetBacklight()->SetBrightness(level, false);
    if (final) {
        // Backlight::SetBrightness 遇到相同的值会直接返回不存盘，所以松手时自己写
        Settings settings("display", true);
        settings.SetInt("brightness", level);
    }
}

int GetBrightness() { return Board::GetInstance().GetBacklight()->brightness(); }

void ReconfigureWifi() {
    Application::GetInstance().Schedule(
        []() { static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode(); });
}

const char* GetConfigUrl() {
#if CONFIG_DIRECT_API
    return DirectApiConfigServer::Url().c_str();
#else
    return "";
#endif
}

void UserActivity() {
    if (NomiDisplay::on_user_activity) NomiDisplay::on_user_activity();
}

void TakePhoto() {
    // 拍照会阻塞几百毫秒，放到主循环里做，别卡住 LVGL 任务
    Application::GetInstance().Schedule([]() {
        if (NomiDisplay::on_take_photo) NomiDisplay::on_take_photo();
    });
}

const char* GetAbout() {
    static char about[96];
    snprintf(about, sizeof(about), "Pocket Companion %s", esp_app_get_description()->version);
    return about;
}

}  // namespace

std::function<void()> NomiDisplay::on_user_activity;
std::function<void()> NomiDisplay::on_take_photo;

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

    DisplayLockGuard lock(this);
    face_buf_ = static_cast<uint16_t*>(heap_caps_malloc(width_ * height_ * sizeof(uint16_t),
                                                        MALLOC_CAP_SPIRAM));
    if (face_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate face buffer, keep XiaoZhi UI");
        return;
    }

    // 小智的界面对象照常存在（它的其他代码还会更新它们），只是全部隐藏
    lv_obj_t* screen = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
        lv_obj_add_flag(lv_obj_get_child(screen, i), LV_OBJ_FLAG_HIDDEN);
    }

    static const nomi_ui_callbacks_t callbacks = {
        .get_state = GetState,
        .toggle_chat = ToggleChat,
        .set_volume = SetVolume,
        .set_brightness = SetBrightness,
        .reconfigure_wifi = ReconfigureWifi,
        .get_volume = GetVolume,
        .get_brightness = GetBrightness,
        .get_config_url = GetConfigUrl,
        .get_about = GetAbout,
        .user_activity = UserActivity,
        .take_photo = TakePhoto,
    };
    // 先用内置字体，资源分区里的完整中文字体加载后会经 SetTextFont 换上
    nomi_ui_create(screen, face_buf_, &BUILTIN_TEXT_FONT, &callbacks);
    ui_ready_ = true;
}

bool NomiDisplay::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    bool ok = SpiLcdDisplay::SetTextFont(text_font);
    if (ok && ui_ready_ && text_font && text_font->font()) {
        DisplayLockGuard lock(this);
        nomi_ui_set_font(text_font->font());
    }
    return ok;
}

void NomiDisplay::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    nomi_ui_set_status(status);
}

void NomiDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!ui_ready_) {
        SpiLcdDisplay::ShowNotification(notification, duration_ms);
        return;
    }
    DisplayLockGuard lock(this);
    nomi_ui_notify(notification, duration_ms);
}

void NomiDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void NomiDisplay::SetEmotion(const char* emotion) {
    if (!ui_ready_) {
        SpiLcdDisplay::SetEmotion(emotion);
        return;
    }
    DisplayLockGuard lock(this);
    nomi_ui_set_emotion(emotion);
}

void NomiDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    // 系统消息（请稍候、错误提示等）用通知条，字幕只留给对话
    if (role != nullptr && strcmp(role, "system") == 0) {
        if (content != nullptr && *content) nomi_ui_notify(content, 4000);
        return;
    }
    nomi_ui_set_subtitle(role, content);
}

void NomiDisplay::UpdateStatusBar(bool update_all) {
    SpiLcdDisplay::UpdateStatusBar(update_all);
    if (!ui_ready_) return;
    int level = -1;
    bool charging = false, discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        level = -1;
    }
    DisplayLockGuard lock(this);
    nomi_ui_set_battery(level, charging);
}

void NomiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    SpiLcdDisplay::SetPreviewImage(std::move(image));
    if (!ui_ready_ || preview_image_ == nullptr) return;
    // 小智的预览图在我们的界面下面，提到最上层；它自己的定时器到时会再隐藏
    DisplayLockGuard lock(this);
    lv_obj_move_foreground(preview_image_);
}

void NomiDisplay::SetSleep(bool sleeping) {
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    nomi_ui_set_sleep(sleeping);
}
