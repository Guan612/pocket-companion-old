#include "adc_battery_monitor.h"
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "mcp_server.h"
#include "esp32_camera.h"
#include "nomi_display.h"
#include "power_save_timer.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st7796.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#define TAG "SpotpearMax35"

class SpotpearMax35Board : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    NomiDisplay* display_ = nullptr;
    AdcBatteryMonitor* battery_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    Esp32Camera* camera_ = nullptr;

    // ES8311、GT911、摄像头 SCCB 共用这条总线
    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));  // 没有复位脚时发软复位命令
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new NomiDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                   DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // INT 没接 GPIO，GT911 上电时的地址可能是 0x5D 也可能是 0x14，两个都探一下
    void InitializeTouch() {
        uint16_t addr = 0;
        for (uint16_t candidate : {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                                   ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP}) {
            if (i2c_master_probe(i2c_bus_, candidate, 50) == ESP_OK) {
                addr = candidate;
                break;
            }
        }
        if (addr == 0) {
            ESP_LOGW(TAG, "GT911 not found, touch disabled");
            return;
        }

        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_config.dev_addr = addr;
        tp_io_config.scl_speed_hz = 400 * 1000;
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create touch IO");
            return;
        }

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = TOUCH_X_MAX;
        tp_cfg.y_max = TOUCH_Y_MAX;
        tp_cfg.rst_gpio_num = TOUCH_RST_PIN;
        tp_cfg.int_gpio_num = TOUCH_INT_PIN;
        tp_cfg.flags.swap_xy = TOUCH_SWAP_XY;
        tp_cfg.flags.mirror_x = TOUCH_MIRROR_X;
        tp_cfg.flags.mirror_y = TOUCH_MIRROR_Y;
        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create GT911 driver");
            return;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "GT911 touch ready at 0x%02X", addr);
    }

    void InitializeBattery() {
        battery_ = new AdcBatteryMonitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL, BATTERY_UPPER_RESISTOR,
                                         BATTERY_LOWER_RESISTOR, BATTERY_USB_DETECT_PIN);
        // 插拔 USB 时立刻刷新电量显示，并重置休眠计时
        battery_->OnChargingStatusChanged([this](bool charging) {
            power_save_timer_->WakeUp();
            display_->UpdateStatusBar(true);
        });
    }

    // cpu_max_freq 传 -1：休眠时不降频，唤醒词保持监听
    void InitializePowerSave() {
        power_save_timer_ = new PowerSaveTimer(-1, IDLE_SECONDS_TO_DIM, IDLE_SECONDS_TO_OFF);
        power_save_timer_->OnEnterSleepMode([this]() {
            display_->SetSleep(true);
            GetBacklight()->SetBrightness(DIM_BRIGHTNESS);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            display_->SetSleep(false);
            GetBacklight()->RestoreBrightness();
        });
        // 到点只息屏，不真关机（这块板没有软关机电路）
        power_save_timer_->OnShutdownRequest([this]() { GetBacklight()->SetBrightness(0); });
        power_save_timer_->SetEnabled(true);
        NomiDisplay::on_user_activity = [this]() { power_save_timer_->WakeUp(); };
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_sccb_sda = -1;  // 复用已初始化的 I2C 总线
        config.pin_sccb_scl = -1;
        config.sccb_i2c_port = I2C_NUM_0;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.xclk_freq_hz = CAMERA_XCLK_HZ;
        config.ledc_timer = LEDC_TIMER_1;  // 0 号被背光 PWM 占了
        config.ledc_channel = LEDC_CHANNEL_1;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new Esp32Camera(config);

        NomiDisplay::on_take_photo = [this]() {
            power_save_timer_->WakeUp();
            if (!camera_->Capture()) {
                display_->ShowNotification("拍照失败，检查摄像头排线", 4000);
            }
        };
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
                           "End this conversation and enter WiFi configuration mode.\n"
                           "**CAUTION** You must ask the user to confirm this action.",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });
    }

public:
    SpotpearMax35Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializePowerSave();
        InitializeBattery();
        InitializeCamera();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Camera* GetCamera() override { return camera_; }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = battery_->IsCharging();
        discharging = !charging;
        level = battery_->GetBatteryLevel();
        return true;
    }

    // 进入对话等高功耗状态时说明有人在用，重置休眠计时
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(SpotpearMax35Board);
