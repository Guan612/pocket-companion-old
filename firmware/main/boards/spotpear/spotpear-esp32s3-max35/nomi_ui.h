// Nomi 界面：全屏表情 + 状态栏 + 字幕 + 通知 + 长按设置菜单。
//
// 只依赖 LVGL 和 face.c，设备（NomiDisplay）和浏览器模拟器（tools/device-sim）共用同一份代码。
// 和平台相关的动作（开始对话、调音量等）通过回调交给调用方。
// 所有函数都要在持有 LVGL 锁的上下文里调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOMI_STATE_IDLE,
    NOMI_STATE_CONNECTING,
    NOMI_STATE_LISTENING,
    NOMI_STATE_SPEAKING,
    NOMI_STATE_ERROR,
} nomi_state_t;

typedef struct {
    nomi_state_t (*get_state)(void);
    void (*toggle_chat)(void);         // 点一下屏幕：开始 / 结束对话
    // 0..100；拖动过程中 final 为 false，松手时为 true（设备只在松手时写 NVS）
    void (*set_volume)(int volume, bool final);
    void (*set_brightness)(int level, bool final);
    void (*reconfigure_wifi)(void);
    int (*get_volume)(void);
    int (*get_brightness)(void);
    const char *(*get_config_url)(void);  // 网页设置页地址，可能为空
    const char *(*get_about)(void);       // 关于页的文字
} nomi_ui_callbacks_t;

// face_buf：480×320 RGB565 画布缓冲，由调用方分配；font：中文字体
void nomi_ui_create(lv_obj_t *screen, uint16_t *face_buf, const lv_font_t *font,
                    const nomi_ui_callbacks_t *callbacks);

// 换字体（比如资源分区加载出完整中文字体之后）
void nomi_ui_set_font(const lv_font_t *font);

void nomi_ui_set_status(const char *status);
// role 为 "user" / "assistant" / "system"；text 为空时隐藏字幕
void nomi_ui_set_subtitle(const char *role, const char *text);
void nomi_ui_set_emotion(const char *emotion);
void nomi_ui_notify(const char *text, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
