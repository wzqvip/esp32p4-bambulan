// 界面内部共用：配色、字体、设置页接口
//
// 注意：不要加一直在跑的动画（呼吸灯、循环闪烁、整屏滑动等）。这块屏在持续刷新时整屏亮度会闪，
// 加过“打印中标签呼吸”就把闪烁带回来了。界面只在内容变化时局部刷新。刷屏方式见 board.c。
#pragma once

#include "lvgl.h"
#include "ui.h"

#define C_BG lv_color_hex(0x0F1317)
#define C_CARD lv_color_hex(0x1B222A)
#define C_CARD2 lv_color_hex(0x252E38)
#define C_TEXT lv_color_hex(0xE8EDF2)
#define C_MUTED lv_color_hex(0x8A96A3)
#define C_GREEN lv_color_hex(0x00AE42)
#define C_BLUE lv_color_hex(0x3D8BFD)
#define C_WARN lv_color_hex(0xF5A623)
#define C_RED lv_color_hex(0xE5484D)
#define C_TEAL lv_color_hex(0x2BB3A3)
#define C_BLACK lv_color_hex(0x000000)
#define C_HEADER lv_color_hex(0x0E3A24)   // 顶栏左侧的墨绿

#define FONT_S (&ui_font_cn_20)
#define FONT_L (&ui_font_cn_28)
#define FONT_NUM (&ui_font_num_56)

/** 创建设置页（ui_init 里调用一次） */
void ui_settings_build(void);
/** 打开设置页，返回时回到 back_to */
void ui_settings_open(lv_obj_t *back_to);
/** 每 250ms 调一次：处理自动息屏。any_printing：有打印机在打印；wake：有值得亮屏的事件（如打印状态变化） */
void ui_settings_tick(bool any_printing, bool wake);
/** 切页动画时长（ms），0 = 直接切换 */
uint32_t ui_page_anim_ms(void);
/** 调试：打开设置页 / 切到下一个分区 */
void ui_settings_debug_next(void);
/** 顶栏底色（墨绿渐变 + 绿色细线），在屏幕上其他控件之前或之后调用都行（会放到最底层） */
void ui_header_band(lv_obj_t *scr);
