// 调试：渲染耗时 / 帧率统计，和自动来回切页。都由 menuconfig「调试」里的开关控制，默认全关。
//
// 每帧拆成几段（单位 ms）：
//   pre    = REFR_START → RENDER_START：布局
//   render = RENDER_START → 最后一次 FLUSH_START：LVGL 软件绘制（含等上一块 DMA 送屏完成的时间）
//   wait   = 本帧所有 FLUSH_WAIT_START → FLUSH_WAIT_FINISH 之和：CPU 等 DMA2D 把画好的区域拷进帧缓冲
//   所以 render - wait ≈ 纯计算；wait 大说明瓶颈在送屏
// 切页动画期间（存在 prev_scr）的帧单独统计，动画结束后打印一行汇总和逐帧耗时。
#include "ui.h"
#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_BBL_DEBUG_PERF
static const char *TAG = "perf";

typedef struct {
    int frames;
    int64_t pre_us, render_us, wait_us, px;
    int64_t max_render_us, max_frame_us;
    int64_t first_us, last_us;
    char list[160];   // 动画逐帧：render+wait
    size_t list_len;
} stats_t;

static stats_t s_idle, s_anim;
static int64_t t_refr, t_render, t_flush, t_flush_end, t_wait0, t_wait_sum, s_last_idle_log;
static int64_t frame_px;
static bool rendered, in_anim;

static void stats_reset(stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void stats_add(stats_t *s, int64_t now, bool keep_list)
{
    int64_t pre = t_render - t_refr, render = t_flush - t_render, wait = t_wait_sum;
    int64_t frame = now - t_refr;
    if (!s->frames) s->first_us = t_refr;
    s->last_us = now;
    s->frames++;
    s->pre_us += pre;
    s->render_us += render;
    s->wait_us += wait;
    s->px += frame_px;
    if (render > s->max_render_us) s->max_render_us = render;
    if (frame > s->max_frame_us) s->max_frame_us = frame;
    if (keep_list && s->list_len + 16 < sizeof(s->list)) {
        s->list_len += snprintf(s->list + s->list_len, sizeof(s->list) - s->list_len, " %d+%d",
                                (int)(render / 1000), (int)(wait / 1000));
    }
}

static void stats_log(const char *what, const stats_t *s)
{
    if (!s->frames) return;
    int64_t span = s->last_us - s->first_us;
    int n = s->frames;
    ESP_LOGI(TAG, "%s: %d frames in %d ms (%.1f fps) | avg pre %.1f render %.1f wait %.1f ms | max render %.1f frame %.1f ms | avg %d kpx",
             what, n, (int)(span / 1000), span > 0 ? n * 1e6 / span : 0.0, s->pre_us / 1000.0 / n,
             s->render_us / 1000.0 / n, s->wait_us / 1000.0 / n, s->max_render_us / 1000.0,
             s->max_frame_us / 1000.0, (int)(s->px / n / 1000));
    if (s->list_len) ESP_LOGI(TAG, "%s per frame (render+wait ms):%s", what, s->list);
}

static void disp_event(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        t_refr = t_render = t_flush = t_flush_end = now;
        t_wait_sum = 0;
        frame_px = 0;
        rendered = false;
        break;
    case LV_EVENT_RENDER_START:
        t_render = now;
        break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *a = lv_event_get_param(e);
        if (a) frame_px += (int64_t)lv_area_get_width(a) * lv_area_get_height(a);
        t_flush = now;
        break;
    }
    case LV_EVENT_FLUSH_WAIT_START:
        t_wait0 = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        t_wait_sum += now - t_wait0;
        break;
    case LV_EVENT_FLUSH_FINISH:
        t_flush_end = now;
        break;
    case LV_EVENT_RENDER_READY:
        rendered = true;
        break;
    case LV_EVENT_REFR_READY: {
        bool anim = ui_slide_busy();
        if (in_anim && !anim) {
            if (rendered) stats_add(&s_anim, now, true);   // 动画最后一帧（prev_scr 已清掉）
            ESP_LOGI(TAG, "page anim: snapshot prep %u ms", (unsigned)ui_slide_prep_ms());
            stats_log("page anim", &s_anim);
            stats_reset(&s_anim);
            in_anim = false;
            break;
        }
        in_anim = anim;
        if (!rendered) break;
        stats_add(anim ? &s_anim : &s_idle, now, anim);
        if (!anim && now - s_last_idle_log > 5000000) {
            s_last_idle_log = now;
            stats_log("idle", &s_idle);
            stats_reset(&s_idle);
        }
        break;
    }
    default:
        break;
    }
}
#endif

#if CONFIG_BBL_DEBUG_PAGE_CYCLE
static void cycle_cb(lv_timer_t *t)
{
    ui_debug_toggle_page();
}
#endif

#if CONFIG_BBL_DEBUG_SETTINGS_TOUR
static void tour_cb(lv_timer_t *t)
{
    ui_settings_debug_next();
}
#endif

void ui_debug_init(lv_display_t *disp)
{
#if CONFIG_BBL_DEBUG_PERF
    if (disp) lv_display_add_event_cb(disp, disp_event, LV_EVENT_ALL, NULL);
#endif
#if CONFIG_BBL_DEBUG_PAGE_CYCLE
    lv_timer_create(cycle_cb, 3000, NULL);
#endif
#if CONFIG_BBL_DEBUG_SETTINGS_TOUR
    lv_timer_create(tour_cb, 5000, NULL);
#endif
    (void)disp;
}
