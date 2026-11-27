#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_cn_20);
LV_FONT_DECLARE(ui_font_cn_28);
LV_FONT_DECLARE(ui_font_num_56);

/** 创建界面并启动刷新定时器。必须在持有 lvgl_port_lock 时调用。 */
void ui_init(void);
