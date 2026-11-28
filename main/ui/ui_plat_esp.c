// 界面用到的平台功能（板子上）。模拟器里在 test/sim/sim_main.c 打桩。
#include "board.h"
#include "esp_heap_caps.h"
#include "ui_priv.h"

void *ui_plat_alloc(size_t size) { return heap_caps_aligned_alloc(128, size, MALLOC_CAP_SPIRAM); }

bool ui_plat_capture_screen(void *dst, size_t size) { return board_copy_framebuffer(dst, size); }
