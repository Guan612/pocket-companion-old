// ST7796S 屏幕：初始化，以及把帧缓冲里的一块区域刷到屏上。
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t display_init(void);

// fb 是整屏 RGB565（本机字节序），只刷 [x0,x1)×[y0,y1) 这一块
void display_flush(const uint16_t *fb, int x0, int y0, int x1, int y1);
