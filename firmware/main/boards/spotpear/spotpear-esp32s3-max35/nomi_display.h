// Nomi 风格表情显示：在小智的 LCD 界面底下铺一张全屏画布画脸，
// 保留小智自带的状态栏和字幕，表情由设备状态和 LLM 情绪共同决定。
#pragma once

#include "display/lcd_display.h"
#include "face.h"

class NomiDisplay : public SpiLcdDisplay {
public:
    NomiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                bool swap_xy);

    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetTheme(Theme* theme) override;

private:
    lv_obj_t* canvas_ = nullptr;
    uint16_t* face_buf_ = nullptr;
    uint32_t last_ms_ = 0;
    float acc_ = 0;
    face_mode_t emotion_mode_ = FACE_IDLE;
    uint32_t emotion_until_ = 0;

    static void OnTick(lv_timer_t* timer);
    void Tick();
    face_mode_t PickMode(uint32_t now) const;
    void MakeChromeTransparent();
};
