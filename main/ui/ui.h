#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_cn_20);
LV_FONT_DECLARE(ui_font_cn_28);
LV_FONT_DECLARE(ui_font_num_56);

/** 创建界面并启动刷新定时器。必须在持有 lvgl_port_lock 时调用。 */
void ui_init(void);

/** 调试用：在总览和详情页之间切换一次（详情页轮流显示每台打印机）。需持有 lvgl_port_lock。 */
void ui_debug_toggle_page(void);

/** 调试用：按 menuconfig 的调试开关启用帧率统计 / 自动切页（ui_perf.c）。需持有 lvgl_port_lock。 */
void ui_debug_init(lv_display_t *disp);
