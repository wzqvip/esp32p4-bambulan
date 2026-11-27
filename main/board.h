// ELECROW CrowPanel Advanced 7" ESP32-P4：EK79007 MIPI-DSI 1024x600 + GT911 触摸 + PWM 背光
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

/** 初始化 LDO、背光、屏幕、触摸和 LVGL（esp_lvgl_port）。成功后可以用 lvgl_port_lock 操作 LVGL。 */
esp_err_t board_display_init(lv_display_t **out_disp);
esp_err_t board_set_backlight(uint32_t percent);
