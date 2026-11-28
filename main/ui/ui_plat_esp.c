// 界面用到的平台功能（板子上）。模拟器里在 test/sim/sim_main.c 打桩。
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ui_priv.h"

void *ui_plat_alloc(size_t size) { return heap_caps_aligned_alloc(128, size, MALLOC_CAP_SPIRAM); }

bool ui_plat_capture_screen(void *dst, size_t size) { return board_copy_framebuffer(dst, size); }

int ui_plat_slide(const void *old_img, const void *new_img, int dir, uint32_t ms)
{
    ESP_LOGI("slide", "snapshot prep %u ms", (unsigned)ui_slide_prep_ms());
    return board_slide(old_img, new_img, dir, ms);
}
