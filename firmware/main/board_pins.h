// SpotPear ESP32-S3 3.5 寸 AI 板（ESP32S3-MAX35）引脚定义。
//
// 来源：官方原理图 ESP32S3-3.5inch-AI-1.pdf（芯片为 ESP32-S3R8，内置 8MB PSRAM）。
// 注意：SpotPear 另有一个 sp-esp32-s3-lcd-3.5 的小智配置，引脚和这块板完全不同，别混用。
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

// 屏幕：ST7796S，480×320，4 线 SPI
#define BOARD_LCD_SPI_HOST   SPI2_HOST
#define BOARD_LCD_SCLK       GPIO_NUM_46
#define BOARD_LCD_MOSI       GPIO_NUM_48
#define BOARD_LCD_CS         GPIO_NUM_5
#define BOARD_LCD_DC         GPIO_NUM_47
#define BOARD_LCD_RST        GPIO_NUM_NC  // 板上是 10K + 22uF 的 RC 上电复位，只能软复位
#define BOARD_LCD_BL         GPIO_NUM_42  // 经 APM2307（P 沟道 MOS）驱动
#define BOARD_LCD_BL_ON      0            // 低电平点亮
#define BOARD_LCD_PCLK_HZ    (40 * 1000 * 1000)
#define BOARD_LCD_H_RES      480
#define BOARD_LCD_V_RES      320
// 以下方向和颜色参数是按常见 ST7796S 模组给的初值，上机后如果画面翻转或偏色再调
#define BOARD_LCD_SWAP_XY    true
#define BOARD_LCD_MIRROR_X   true
#define BOARD_LCD_MIRROR_Y   true
#define BOARD_LCD_INVERT     true
#define BOARD_LCD_BGR        true

// 音频：ES8311 编解码 + NS4150B 功放（音频链路尚未接入，引脚先定下来）
#define BOARD_I2S_MCLK       GPIO_NUM_16
#define BOARD_I2S_BCLK       GPIO_NUM_9
#define BOARD_I2S_WS         GPIO_NUM_45
#define BOARD_I2S_DOUT       GPIO_NUM_8   // ESP32 → ES8311 DSDIN
#define BOARD_I2S_DIN        GPIO_NUM_10  // ES8311 ASDOUT → ESP32
#define BOARD_PA_EN          GPIO_NUM_43  // NS4150B CTRL，高电平打开；它也是 U0TXD，日志须走 USB
#define BOARD_PA_ON          1

// I2C：ES8311、GT911 触摸、摄像头 SCCB 共用一条总线
#define BOARD_I2C_PORT       I2C_NUM_0
#define BOARD_I2C_SCL        GPIO_NUM_14
#define BOARD_I2C_SDA        GPIO_NUM_15

// 触摸：GT911。复位脚是 RC 上电复位，INT 在原理图里没接到 GPIO，只能轮询
#define BOARD_TP_INT         GPIO_NUM_NC
#define BOARD_TP_RST         GPIO_NUM_NC

// BOOT 键：切换表情
#define BOARD_BTN_BOOT       GPIO_NUM_0
