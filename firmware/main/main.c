// Pocket Companion 固件入口：先把方形 Nomi 表情跑起来。
//
// BOOT 键切换表情；不按键时每 3.5 秒自动轮播。语音链路（录音、WebSocket、播放）是下一步。
#include "board_pins.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STEP_US (1000000 / 60)  // 动画固定 60Hz 步长
#define FRAME_MS 33             // 约 30fps 刷屏
#define AUTO_CYCLE_US (3500 * 1000)

static const char *TAG = "main";

static bool boot_pressed(void)
{
    static int last = 1;
    int now = gpio_get_level(BOARD_BTN_BOOT);
    bool pressed = last == 1 && now == 0;
    last = now;
    return pressed;
}

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BTN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    uint16_t *fb = heap_caps_malloc(FACE_W * FACE_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!fb) {
        ESP_LOGE(TAG, "PSRAM 帧缓冲分配失败");
        return;
    }

    face_init(esp_random());
    bool auto_cycle = true;
    int64_t last = esp_timer_get_time(), acc = 0, cycle_at = last + AUTO_CYCLE_US;

    while (true) {
        int64_t now = esp_timer_get_time();
        acc += now - last;
        last = now;
        if (acc > 100000) acc = 100000;  // 卡顿后别一次追太多步
        for (; acc >= STEP_US; acc -= STEP_US) face_step(STEP_US / 1e6f);

        face_mode_t mode = face_get_mode();
        if (boot_pressed()) {
            auto_cycle = false;
            face_set_mode((mode + 1) % FACE_MODE_COUNT);
            ESP_LOGI(TAG, "mode -> %s", face_mode_name(face_get_mode()));
        } else if (auto_cycle && now >= cycle_at) {
            cycle_at = now + AUTO_CYCLE_US;
            face_set_mode((mode + 1) % FACE_MODE_COUNT);
        }

        rrect_t d = face_render(fb);
        if (!rrect_empty(d)) display_flush(fb, d.x0, d.y0, d.x1, d.y1);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}
