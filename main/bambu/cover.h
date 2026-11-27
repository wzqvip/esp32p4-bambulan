// 打印任务的模型预览图：经 FTPS（990 端口，隐式 TLS）从打印机读取正在打印的 3mf，
// 只按需读 ZIP 尾部目录和 Metadata/plate_N.png 这一项（不下载整个文件），
// 解码 PNG 后缩成 COVER_SIZE 见方的 ARGB8888，绑定到同一台打印机的 lv_image。
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#define COVER_SIZE 128
#define COVER_MAX_BIND 2

/** 把 lv_image 绑定到第 idx 台打印机的预览图（在 LVGL 锁内、cover_start_all 之前调用）。 */
void cover_bind_image(int idx, lv_obj_t *img);

/** 网络就绪后调用：为每台已配置的打印机启动后台任务，任务变化时自动取图。 */
void cover_start_all(void);

/** 当前任务有预览图 */
bool cover_has_image(int idx);
