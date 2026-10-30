#include "display.h"

#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FLUSH_LINES 40  // 每次 DMA 传输的行数

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_line_buf;
static SemaphoreHandle_t s_trans_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    gpio_config_t bl = {.pin_bit_mask = 1ULL << BOARD_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(BOARD_LCD_BL, !BOARD_LCD_BL_ON);  // 先关背光，避免上电花屏

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BOARD_LCD_H_RES * FLUSH_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    s_trans_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, BOARD_LCD_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));

    s_line_buf = heap_caps_malloc(BOARD_LCD_H_RES * FLUSH_LINES * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_line_buf) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    gpio_set_level(BOARD_LCD_BL, BOARD_LCD_BL_ON);
    ESP_LOGI(TAG, "ST7796S %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

void display_flush(const uint16_t *fb, int x0, int y0, int x1, int y1)
{
    const int w = x1 - x0;
    if (w <= 0 || y1 <= y0) return;
    for (int y = y0; y < y1; y += FLUSH_LINES) {
        int rows = y1 - y < FLUSH_LINES ? y1 - y : FLUSH_LINES;
        // 帧缓冲在 PSRAM 里且是本机小端，屏幕要大端，拷到 DMA 行缓冲时顺便换字节
        for (int r = 0; r < rows; r++) {
            const uint16_t *src = fb + (y + r) * BOARD_LCD_H_RES + x0;
            uint16_t *dst = s_line_buf + r * w;
            for (int i = 0; i < w; i++) dst[i] = (uint16_t)((src[i] << 8) | (src[i] >> 8));
        }
        esp_lcd_panel_draw_bitmap(s_panel, x0, y, x1, y + rows, s_line_buf);
        xSemaphoreTake(s_trans_done, portMAX_DELAY);
    }
}
