// 切页滑动动画：先把新旧两页各“拍”成一张整屏图片，动画期间只移动这两张图片。
//
// 为什么：LVGL 自带的 lv_screen_load_anim 每一帧都要把两页上所有控件（几十个中文 label、圆角框、
// 摄像头画面、缩放的预览图……）重新软件绘制一遍，实测一帧 35~50ms，只有 18~23fps。
// 改成两张图以后，每帧只剩两次整块 RGB565 拷贝（无缩放、无混合，LVGL 走 memcpy），
// 送屏仍由 esp_lvgl_port 用 DMA2D 拷进帧缓冲。
//
// 旧页面：板子上直接从帧缓冲复制（它就是屏幕上正在显示的内容），模拟器里用 lv_snapshot 渲染。
// 新页面：用 lv_snapshot 离屏渲染一次（只这一次是完整绘制）。
#include <string.h>

#include "ui_priv.h"

#define W 1024
#define H 600

static lv_obj_t *s_scr, *s_img_old, *s_img_new, *s_target;
static lv_draw_buf_t s_buf_old, s_buf_new;
static bool s_ready, s_busy;
static int s_dir;
static uint32_t s_prep_ms;   // 上次切页准备两张快照用了多久   // +1：新页从右边进来；-1：从左边进来

static bool init_bufs(void)
{
    if (s_ready) return true;
    size_t stride = W * 2, size = stride * H;
    void *a = ui_plat_alloc(size), *b = ui_plat_alloc(size);
    if (!a || !b) return false;
    lv_draw_buf_init(&s_buf_old, W, H, LV_COLOR_FORMAT_RGB565, stride, a, size);
    lv_draw_buf_init(&s_buf_new, W, H, LV_COLOR_FORMAT_RGB565, stride, b, size);

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_img_old = lv_image_create(s_scr);
    s_img_new = lv_image_create(s_scr);
    lv_obj_remove_flag(s_img_old, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_img_new, LV_OBJ_FLAG_CLICKABLE);
    s_ready = true;
    return true;
}

static void anim_x(void *var, int32_t x)
{
    lv_obj_set_x(s_img_old, x);
    lv_obj_set_x(s_img_new, x + s_dir * W);
}

static void anim_done(lv_anim_t *a)
{
    lv_screen_load(s_target);
    s_busy = false;
}

bool ui_slide_busy(void) { return s_busy; }

uint32_t ui_slide_prep_ms(void) { return s_prep_ms; }

void ui_slide_to(lv_obj_t *target, bool forward)
{
    uint32_t ms = ui_page_anim_ms();
    if (s_busy) return;
    if (ms == 0 || !init_bufs()) {
        lv_screen_load(target);
        return;
    }
    uint32_t t0 = lv_tick_get();
    lv_obj_t *cur = lv_screen_active();
    // 旧页：优先直接复制帧缓冲
    if (!ui_plat_capture_screen(s_buf_old.data, s_buf_old.data_size)) {
        if (lv_snapshot_take_to_draw_buf(cur, LV_COLOR_FORMAT_RGB565, &s_buf_old) != LV_RESULT_OK) {
            lv_screen_load(target);
            return;
        }
    }
    lv_obj_update_layout(target);
    if (lv_snapshot_take_to_draw_buf(target, LV_COLOR_FORMAT_RGB565, &s_buf_new) != LV_RESULT_OK) {
        lv_screen_load(target);
        return;
    }
    s_prep_ms = lv_tick_elaps(t0);
    lv_image_cache_drop(&s_buf_old);
    lv_image_cache_drop(&s_buf_new);
    lv_image_set_src(s_img_old, &s_buf_old);
    lv_image_set_src(s_img_new, &s_buf_new);
    s_dir = forward ? 1 : -1;
    s_target = target;
    anim_x(NULL, 0);
    s_busy = true;
    lv_screen_load(s_scr);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_scr);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_values(&a, 0, -s_dir * W);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}
