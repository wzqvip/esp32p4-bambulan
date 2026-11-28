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
/** 右下角性能信息（ui_stats.c，只在板子上编译） */
void ui_stats_init(lv_display_t *disp);
/** 切页：设置里开了动画就滑过去（快照 + 移动两张整屏图），否则直接切换。forward=新页从右边进来 */
void ui_slide_to(lv_obj_t *target, bool forward);
bool ui_slide_busy(void);
uint32_t ui_slide_prep_ms(void);
int ui_slide_hw_frames(void);

// ---- 平台相关（板子上在 ui_plat_esp.c，模拟器里打桩） ----
/** 分配一块大内存（板子上放 PSRAM，按 cache line 对齐） */
void *ui_plat_alloc(size_t size);
/** 把屏幕当前显示的内容复制到 dst（RGB565，1024x600）。做不到返回 false，由调用方用 lv_snapshot 代替 */
bool ui_plat_capture_screen(void *dst, size_t size);
/** 硬件切页滑动（板子上用 PPA 直接写帧缓冲）。返回画的帧数；不支持/失败返回 -1，界面会退回 LVGL 动画。 */
int ui_plat_slide(const void *old_img, const void *new_img, int dir, uint32_t ms);
