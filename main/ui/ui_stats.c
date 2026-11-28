// 右下角小字性能信息：FPS、1% low、平均渲染耗时、两个核的 CPU 占用。
// 参考 LVGL 官方 sysmon / perf monitor 的做法（挂在 sys layer 上的一个 label），但有两点不同：
//  1. 只在文字变化时才改 label，空闲时不会因为它自己每秒刷新而一直重画；
//  2. 只统计“真的有内容变化”的帧：只有这个 label 自己变了的帧不算进 FPS。
// 定义：
//   FPS     = 最近 1 秒实际重画的帧数（界面不动时应该是 0）
//   1% low  = 最近 256 帧里最慢 1% 那一帧（第 99 百分位的帧耗时）换算成的帧率
//   render  = 最近 1 秒每帧平均耗时（REFR_START → REFR_READY，含绘制和送屏）
//   CPU     = 每个核 1 - 空闲任务占比（FreeRTOS 运行时间统计）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "settings.h"
#include "ui_priv.h"

#define RING 256

static lv_obj_t *s_lbl;
static int64_t s_t0;
static bool s_dirty_other;   // 自上一帧以来，有 overlay 以外的区域失效
static uint16_t s_ring[RING];   // 帧耗时，单位 0.1ms
static int s_ring_n, s_ring_pos;
static int s_frames;
static int64_t s_frame_us_sum;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static configRUN_TIME_COUNTER_TYPE s_idle_prev[2];
#endif
static int64_t s_cpu_t_prev;

static void disp_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_INVALIDATE_AREA: {
        const lv_area_t *a = lv_event_get_param(e);
        if (!s_lbl || !a) break;
        lv_area_t c;
        lv_obj_get_coords(s_lbl, &c);
        if (lv_obj_has_flag(s_lbl, LV_OBJ_FLAG_HIDDEN) || !(a->x1 >= c.x1 && a->y1 >= c.y1 && a->x2 <= c.x2 && a->y2 <= c.y2)) s_dirty_other = true;
        break;
    }
    case LV_EVENT_REFR_START:
        s_t0 = esp_timer_get_time();
        break;
    case LV_EVENT_REFR_READY: {
        if (!s_dirty_other) break;   // 没画东西，或只画了 overlay 自己
        s_dirty_other = false;
        int64_t us = esp_timer_get_time() - s_t0;
        s_frames++;
        s_frame_us_sum += us;
        int v = (int)(us / 100);
        s_ring[s_ring_pos] = v > 65535 ? 65535 : (uint16_t)v;
        s_ring_pos = (s_ring_pos + 1) % RING;
        if (s_ring_n < RING) s_ring_n++;
        break;
    }
    default:
        break;
    }
}

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

static void cpu_usage(int out[2])
{
    out[0] = out[1] = -1;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_cpu_t_prev;
    for (int i = 0; i < 2 && i < portNUM_PROCESSORS; i++) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(i);
        if (s_cpu_t_prev && dt > 0) {
            int64_t busy = 100 - (int64_t)(configRUN_TIME_COUNTER_TYPE)(idle - s_idle_prev[i]) * 100 / dt;
            out[i] = busy < 0 ? 0 : busy > 100 ? 100 : (int)busy;
        }
        s_idle_prev[i] = idle;
    }
    s_cpu_t_prev = now;
#endif
}

static void tick_cb(lv_timer_t *t)
{
    int cpu[2];
    cpu_usage(cpu);
    if (!settings_get()->perf_overlay) {
        if (!lv_obj_has_flag(s_lbl, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_lbl, LV_OBJ_FLAG_HIDDEN);
        s_frames = 0;
        s_frame_us_sum = 0;
        return;
    }
    if (lv_obj_has_flag(s_lbl, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_lbl, LV_OBJ_FLAG_HIDDEN);

    char low[16] = "-";
    if (s_ring_n >= 10) {
        uint16_t tmp[RING];
        memcpy(tmp, s_ring, s_ring_n * sizeof(tmp[0]));
        qsort(tmp, s_ring_n, sizeof(tmp[0]), cmp_u16);
        int p99 = tmp[(s_ring_n * 99) / 100];
        if (p99 > 0) snprintf(low, sizeof(low), "%d", 10000 / p99);
    }
    char render[16] = "-";
    if (s_frames) snprintf(render, sizeof(render), "%.1fms", s_frame_us_sum / 1000.0 / s_frames);
    char buf[96];
    snprintf(buf, sizeof(buf), "FPS %d  1%%low %s  render %s  CPU %d%% %d%%", s_frames, low, render, cpu[0], cpu[1]);
    s_frames = 0;
    s_frame_us_sum = 0;
    if (strcmp(lv_label_get_text(s_lbl), buf) != 0) lv_label_set_text(s_lbl, buf);
}

void ui_stats_init(lv_display_t *disp)
{
    s_lbl = lv_label_create(lv_layer_sys());
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl, C_MUTED, 0);
    // 固定宽度、右对齐：文字长短变化时 label 本身的区域不变，重画只限于这一小块
    lv_obj_set_width(s_lbl, 420);
    lv_obj_set_style_text_align(s_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_lbl, "FPS -");
    lv_obj_align(s_lbl, LV_ALIGN_BOTTOM_RIGHT, -14, 2);
    lv_obj_remove_flag(s_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_display_add_event_cb(disp, disp_event, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, disp_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, disp_event, LV_EVENT_REFR_READY, NULL);
    lv_timer_create(tick_cb, 1000, NULL);
}
