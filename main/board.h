// ELECROW CrowPanel Advanced 7" ESP32-P4：EK79007 MIPI-DSI 1024x600 + GT911 触摸 + PWM 背光
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

/** 初始化 LDO、背光、屏幕、触摸和 LVGL（esp_lvgl_port）。成功后可以用 lvgl_port_lock 操作 LVGL。 */
esp_err_t board_display_init(lv_display_t **out_disp);
esp_err_t board_set_backlight(uint32_t percent);
/** 把当前帧缓冲（屏幕正在显示的内容，RGB565）复制到 dst */
bool board_copy_framebuffer(void *dst, size_t size);
/** 切页滑动：用 PPA 按 vsync 把两张整屏 RGB565 快照直接拷进帧缓冲（不经过 LVGL 绘制）。
 *  dir>0 新页从右边进来，<0 从左边进来。阻塞 ms 毫秒；返回画了多少帧，失败返回 -1。
 *  调用期间必须拿着 LVGL 锁（不能有 LVGL 刷屏同时进行）。 */
int board_slide(const void *old_img, const void *new_img, int dir, uint32_t ms);
