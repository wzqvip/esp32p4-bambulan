// 设置页：显示（亮度、自动息屏、动画、实时画面）、时间（时区）、Wi-Fi、打印机、关于。
// 显示和时间改了立即生效并保存；Wi-Fi 和打印机改完点“保存并重启”才生效。
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "net.h"
#include "printer.h"
#include "settings.h"
#include "ui_priv.h"

#define NAV_N 5
enum { SEC_DISPLAY, SEC_TIME, SEC_WIFI, SEC_PRINTER, SEC_ABOUT };
static const char *s_nav_txt[NAV_N] = {LV_SYMBOL_IMAGE "  显示", LV_SYMBOL_REFRESH "  时间", LV_SYMBOL_WIFI "  Wi-Fi",
                                       LV_SYMBOL_LIST "  打印机", LV_SYMBOL_HOME "  关于"};

static const uint16_t s_sleep_vals[] = {0, 1, 2, 5, 10, 30};
static const char *s_sleep_opts = "从不\n1 分钟\n2 分钟\n5 分钟\n10 分钟\n30 分钟";
static const uint8_t s_level_vals[] = {0, 10, 30};
static const char *s_level_opts = "关闭背光\n调暗到 10%\n调暗到 30%";

static lv_obj_t *s_scr, *s_back_to;
static lv_obj_t *s_nav[NAV_N], *s_sec[NAV_N];
static int s_cur_sec;

// 显示
static lv_obj_t *s_bright, *s_bright_val, *s_sleep_dd, *s_level_dd, *s_print_sw, *s_anim_sw, *s_cam_sw, *s_stats_sw;
// 时间
static lv_obj_t *s_now, *s_sync, *s_tz_dd;
// Wi-Fi / 打印机：改的是 s_edit，保存时才写回
static settings_t s_edit;
static lv_obj_t *s_wifi_info, *s_wifi_save;
static lv_obj_t *s_plink[SETTINGS_PRINTERS], *s_printer_save;
// 关于
static lv_obj_t *s_about;
static lv_obj_t *s_msgbox;

// 息屏
static lv_obj_t *s_blocker;
static bool s_asleep;

// ---------- 小工具 ----------
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_color_t bg, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_CARD2, LV_STATE_DISABLED);
    lv_obj_t *l = label(b, FONT_S, C_TEXT, txt);
    lv_obj_set_style_text_color(l, C_MUTED, LV_STATE_DISABLED);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void set_text(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
}

// 一行：左边标题（可带一行小字说明），右边控件
static lv_obj_t *row(lv_obj_t *sec, const char *title, const char *hint)
{
    lv_obj_t *r = plain(sec);
    lv_obj_set_size(r, LV_PCT(100), hint ? 76 : 64);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, C_CARD2, 0);
    lv_obj_t *t = label(r, FONT_S, C_TEXT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, hint ? -12 : 0);
    if (hint) {
        lv_obj_t *h = label(r, FONT_S, C_MUTED, hint);
        lv_obj_set_style_text_font(h, FONT_S, 0);
        lv_obj_align(h, LV_ALIGN_LEFT_MID, 0, 16);
    }
    return r;
}

static lv_obj_t *section_title(lv_obj_t *sec, const char *txt)
{
    lv_obj_t *l = label(sec, FONT_L, C_TEXT, txt);
    lv_obj_set_style_pad_top(l, 4, 0);
    lv_obj_set_style_pad_bottom(l, 8, 0);
    return l;
}

static lv_obj_t *mk_switch(lv_obj_t *r, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *mk_dropdown(lv_obj_t *r, const char *opts, int sel, int w, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(r);
    lv_dropdown_set_options_static(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_width(dd, w);
    lv_obj_set_style_text_font(dd, FONT_S, 0);
    lv_obj_set_style_bg_color(dd, C_CARD2, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), FONT_S, 0);
    lv_obj_set_style_max_height(lv_dropdown_get_list(dd), 400, 0);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

// 立即生效的设置：在当前设置的副本上改一项再保存（不碰还没保存的 Wi-Fi/打印机改动）
#define SAVE_NOW(stmt)                       \
    do {                                     \
        settings_t _s = *settings_get();     \
        settings_t *s = &_s;                 \
        stmt;                                \
        settings_save(s);                    \
    } while (0)

// ---------- 显示 ----------
static void bright_changed(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    set_text(s_bright_val, buf);
    sys_set_backlight(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) SAVE_NOW(s->brightness = (uint8_t)v);
}

static void sleep_changed(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(s_sleep_dd);
    if (i < sizeof(s_sleep_vals) / sizeof(s_sleep_vals[0])) SAVE_NOW(s->sleep_min = s_sleep_vals[i]);
}

static void level_changed(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(s_level_dd);
    if (i < sizeof(s_level_vals)) SAVE_NOW(s->sleep_level = s_level_vals[i]);
}

static void print_sw_changed(lv_event_t *e)
{
    // 开关含义是“打印时保持常亮”
    SAVE_NOW(s->sleep_when_printing = !lv_obj_has_state(s_print_sw, LV_STATE_CHECKED));
}

static void anim_changed(lv_event_t *e) { SAVE_NOW(s->page_anim = lv_obj_has_state(s_anim_sw, LV_STATE_CHECKED)); }

static void stats_changed(lv_event_t *e) { SAVE_NOW(s->perf_overlay = lv_obj_has_state(s_stats_sw, LV_STATE_CHECKED)); }

static void cam_changed(lv_event_t *e) { SAVE_NOW(s->camera_on = lv_obj_has_state(s_cam_sw, LV_STATE_CHECKED)); }

static int index_of_u16(const uint16_t *a, int n, int v)
{
    for (int i = 0; i < n; i++)
        if (a[i] == v) return i;
    return 0;
}

static void build_display(lv_obj_t *sec)
{
    const settings_t *st = settings_get();
    section_title(sec, "显示");

    lv_obj_t *r = row(sec, "屏幕亮度", NULL);
    s_bright_val = label(r, FONT_S, C_MUTED, "");
    lv_obj_align(s_bright_val, LV_ALIGN_RIGHT_MID, 0, 0);
    s_bright = lv_slider_create(r);
    lv_slider_set_range(s_bright, 5, 100);
    lv_slider_set_value(s_bright, st->brightness, LV_ANIM_OFF);
    lv_obj_set_size(s_bright, 360, 14);
    lv_obj_align(s_bright, LV_ALIGN_RIGHT_MID, -80, 0);
    lv_obj_set_ext_click_area(s_bright, 20);
    lv_obj_add_event_cb(s_bright, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bright, bright_changed, LV_EVENT_RELEASED, NULL);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", st->brightness);
    lv_label_set_text(s_bright_val, buf);

    r = row(sec, "自动息屏", "一段时间没有操作后关闭或调暗屏幕，点一下屏幕唤醒");
    s_sleep_dd = mk_dropdown(r, s_sleep_opts, index_of_u16(s_sleep_vals, 6, st->sleep_min), 200, sleep_changed);

    r = row(sec, "息屏方式", NULL);
    int lv = 0;
    for (int i = 0; i < 3; i++)
        if (s_level_vals[i] == st->sleep_level) lv = i;
    s_level_dd = mk_dropdown(r, s_level_opts, lv, 200, level_changed);

    r = row(sec, "打印时保持常亮", "有打印机在打印或暂停时不息屏");
    s_print_sw = mk_switch(r, !st->sleep_when_printing, print_sw_changed);

    r = row(sec, "切页滑动动画", "关闭时直接切换，更跟手");
    s_anim_sw = mk_switch(r, st->page_anim, anim_changed);

    r = row(sec, "摄像头实时画面", "关闭后不再连接打印机摄像头");
    s_cam_sw = mk_switch(r, st->camera_on, cam_changed);

    r = row(sec, "显示帧率和 CPU 占用", "右下角小字：FPS、1% low、每帧耗时、两个核的 CPU");
    s_stats_sw = mk_switch(r, st->perf_overlay, stats_changed);
}

// ---------- 时间 ----------
static char s_tz_opts[640];

static void tz_changed(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(s_tz_dd);
    const char *tz = settings_tz_list[i].tz;
    if (!tz) return;
    SAVE_NOW(snprintf(s->tz, sizeof(s->tz), "%s", tz));
}

static void build_time(lv_obj_t *sec)
{
    section_title(sec, "时间");
    lv_obj_t *r = row(sec, "当前时间", NULL);
    s_now = label(r, FONT_L, C_TEXT, "-");
    lv_obj_align(s_now, LV_ALIGN_RIGHT_MID, 0, 0);

    r = row(sec, "网络对时", "ntp.aliyun.com / pool.ntp.org，联网后自动同步");
    s_sync = label(r, FONT_S, C_MUTED, "-");
    lv_obj_align(s_sync, LV_ALIGN_RIGHT_MID, 0, 0);

    r = row(sec, "时区", NULL);
    size_t n = 0;
    int sel = 0;
    for (int i = 0; settings_tz_list[i].label; i++) {
        n += snprintf(s_tz_opts + n, sizeof(s_tz_opts) - n, "%s%s", i ? "\n" : "", settings_tz_list[i].label);
        if (!strcmp(settings_tz_list[i].tz, settings_get()->tz)) sel = i;
    }
    s_tz_dd = mk_dropdown(r, s_tz_opts, sel, 440, tz_changed);
}

static void update_time(void)
{
    char buf[48];
    if (net_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        static const char *wd[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(buf, sizeof(buf), "%d-%02d-%02d 周%s %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 wd[tm.tm_wday], tm.tm_hour, tm.tm_min, tm.tm_sec);
        set_text(s_now, buf);
        set_text(s_sync, "已同步");
    } else {
        set_text(s_now, "--:--");
        set_text(s_sync, "未同步（等待联网）");
    }
}

// ---------- 文本编辑弹窗 ----------
typedef struct {
    char *dst;
    size_t cap;
    lv_obj_t *val;      // 行上显示值的 label
    bool secret;        // 行上显示成圆点
    lv_obj_t *save_btn; // 改了之后启用的保存按钮
} edit_target_t;

static edit_target_t s_targets[2 + SETTINGS_PRINTERS * 4];
static int s_ntargets;
static lv_obj_t *s_edit_layer, *s_edit_ta;
static edit_target_t *s_editing;

static void show_value(edit_target_t *t)
{
    char buf[80];
    if (!t->dst[0]) snprintf(buf, sizeof(buf), "（未设置）");
    else if (t->secret) snprintf(buf, sizeof(buf), "••••••••");
    else snprintf(buf, sizeof(buf), "%s", t->dst);
    set_text(t->val, buf);
}

static void edit_close(bool apply)
{
    if (apply && s_editing) {
        const char *txt = lv_textarea_get_text(s_edit_ta);
        if (strcmp(txt, s_editing->dst) != 0) {
            snprintf(s_editing->dst, s_editing->cap, "%s", txt);
            show_value(s_editing);
            lv_obj_remove_state(s_editing->save_btn, LV_STATE_DISABLED);
        }
    }
    s_editing = NULL;
    if (s_edit_layer) {
        lv_obj_delete(s_edit_layer);
        s_edit_layer = NULL;
    }
}

static void edit_kb_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) edit_close(true);
    else if (c == LV_EVENT_CANCEL) edit_close(false);
}

static void edit_btn(lv_event_t *e) { edit_close((bool)(intptr_t)lv_event_get_user_data(e)); }

static void edit_open(lv_event_t *e)
{
    edit_target_t *t = lv_event_get_user_data(e);
    lv_obj_t *r = lv_event_get_current_target(e);
    const char *title = lv_label_get_text(lv_obj_get_child(r, 0));
    s_editing = t;

    s_edit_layer = plain(lv_layer_top());
    lv_obj_set_size(s_edit_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_edit_layer, C_BLACK, 0);
    lv_obj_set_style_bg_opa(s_edit_layer, LV_OPA_70, 0);
    lv_obj_add_flag(s_edit_layer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *p = plain(s_edit_layer);
    lv_obj_set_size(p, 900, 190);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(p, C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_t *l = label(p, FONT_L, C_TEXT, title);
    lv_obj_set_pos(l, 0, 0);

    s_edit_ta = lv_textarea_create(p);
    lv_textarea_set_one_line(s_edit_ta, true);
    lv_textarea_set_max_length(s_edit_ta, (uint32_t)t->cap - 1);
    lv_textarea_set_text(s_edit_ta, t->dst);
    lv_obj_set_style_text_font(s_edit_ta, FONT_L, 0);
    lv_obj_set_style_bg_color(s_edit_ta, C_CARD2, 0);
    lv_obj_set_style_border_width(s_edit_ta, 0, 0);
    lv_obj_set_size(s_edit_ta, 560, 64);
    lv_obj_set_pos(s_edit_ta, 0, 60);
    lv_obj_add_state(s_edit_ta, LV_STATE_FOCUSED);

    lv_obj_t *b = button(p, "取消", C_CARD2, 130, 64, edit_btn, (void *)(intptr_t)0);
    lv_obj_set_pos(b, 580, 60);
    b = button(p, "确定", C_GREEN, 130, 64, edit_btn, (void *)(intptr_t)1);
    lv_obj_set_pos(b, 725, 60);

    lv_obj_t *kb = lv_keyboard_create(s_edit_layer);
    lv_obj_set_size(kb, LV_PCT(100), 340);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, FONT_S, 0);
    lv_keyboard_set_textarea(kb, s_edit_ta);
    lv_obj_add_event_cb(kb, edit_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, edit_kb_event, LV_EVENT_CANCEL, NULL);
}

static void edit_row(lv_obj_t *sec, const char *title, char *dst, size_t cap, bool secret, lv_obj_t *save_btn)
{
    edit_target_t *t = &s_targets[s_ntargets++];
    t->dst = dst;
    t->cap = cap;
    t->secret = secret;
    lv_obj_t *r = row(sec, title, NULL);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *chev = label(r, FONT_S, C_MUTED, LV_SYMBOL_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -4, 0);
    t->val = label(r, FONT_S, C_MUTED, "");
    lv_obj_align(t->val, LV_ALIGN_RIGHT_MID, -34, 0);
    t->save_btn = save_btn;
    show_value(t);
    lv_obj_add_event_cb(r, edit_open, LV_EVENT_CLICKED, t);
}

// ---------- 确认框 ----------
static void msgbox_close_cb(lv_event_t *e)
{
    void (*action)(void) = lv_event_get_user_data(e);
    if (s_msgbox) {
        lv_msgbox_close(s_msgbox);
        s_msgbox = NULL;
    }
    if (action) action();
}

static void confirm(const char *title, const char *text, const char *yes_txt, lv_color_t yes_color, void (*action)(void))
{
    if (s_msgbox) return;
    s_msgbox = lv_msgbox_create(NULL);
    lv_obj_set_style_text_font(s_msgbox, FONT_S, 0);
    lv_obj_set_style_bg_color(s_msgbox, C_CARD, 0);
    lv_obj_set_style_radius(s_msgbox, 16, 0);
    lv_obj_set_style_pad_all(s_msgbox, 12, 0);
    lv_obj_set_width(s_msgbox, 600);
    lv_obj_t *t = lv_msgbox_add_title(s_msgbox, title);
    lv_obj_set_style_text_font(t, FONT_L, 0);
    lv_obj_t *x = lv_msgbox_add_text(s_msgbox, text);
    lv_obj_set_style_pad_ver(x, 16, 0);
    lv_obj_t *no = lv_msgbox_add_footer_button(s_msgbox, "取消");
    lv_obj_set_size(no, 160, 56);
    lv_obj_set_style_bg_color(no, C_CARD2, 0);
    lv_obj_add_event_cb(no, msgbox_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_msgbox, yes_txt);
    lv_obj_set_size(yes, 160, 56);
    lv_obj_set_style_bg_color(yes, yes_color, 0);
    lv_obj_add_event_cb(yes, msgbox_close_cb, LV_EVENT_CLICKED, (void *)action);
    lv_obj_set_height(lv_msgbox_get_footer(s_msgbox), 80);
    lv_obj_set_style_bg_opa(lv_msgbox_get_header(s_msgbox), LV_OPA_TRANSP, 0);
}

// ---------- Wi-Fi / 打印机 ----------
static void save_and_restart(void)
{
    settings_t s = *settings_get();
    memcpy(s.wifi_ssid, s_edit.wifi_ssid, sizeof(s.wifi_ssid));
    memcpy(s.wifi_pass, s_edit.wifi_pass, sizeof(s.wifi_pass));
    memcpy(s.printer, s_edit.printer, sizeof(s.printer));
    settings_save(&s);
    sys_restart();
}

static void save_clicked(lv_event_t *e)
{
    confirm("保存并重启", "新的设置需要重启后生效，现在重启吗？", "重启", C_GREEN, save_and_restart);
}

static lv_obj_t *save_button(lv_obj_t *sec)
{
    lv_obj_t *r = plain(sec);
    lv_obj_set_size(r, LV_PCT(100), 88);
    lv_obj_t *b = button(r, LV_SYMBOL_SAVE " 保存并重启", C_GREEN, 220, 56, save_clicked, NULL);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_state(b, LV_STATE_DISABLED);
    return b;
}

static void build_wifi(lv_obj_t *sec)
{
    section_title(sec, "Wi-Fi");
    lv_obj_t *r = row(sec, "状态", NULL);
    s_wifi_info = label(r, FONT_S, C_MUTED, "-");
    lv_obj_align(s_wifi_info, LV_ALIGN_RIGHT_MID, 0, 0);
    s_wifi_save = save_button(sec);   // 编辑行要引用它，先建，最后挪到底部
    edit_row(sec, "网络名称 (SSID)", s_edit.wifi_ssid, sizeof(s_edit.wifi_ssid), false, s_wifi_save);
    edit_row(sec, "密码", s_edit.wifi_pass, sizeof(s_edit.wifi_pass), true, s_wifi_save);
    lv_obj_t *note = label(sec, FONT_S, C_MUTED, "Wi-Fi 由板载 ESP32-C6 连接，只支持 2.4GHz。");
    lv_obj_set_style_pad_top(note, 12, 0);
    lv_obj_move_to_index(lv_obj_get_parent(s_wifi_save), -1);
}

static void build_printers(lv_obj_t *sec)
{
    section_title(sec, "打印机");
    s_printer_save = save_button(sec);
    for (int i = 0; i < printers_count() && i < SETTINGS_PRINTERS; i++) {
        char t[32];
        snprintf(t, sizeof(t), "打印机 %d", i + 1);
        lv_obj_t *r = row(sec, t, NULL);
        lv_obj_set_style_text_color(lv_obj_get_child(r, 0), C_GREEN, 0);
        s_plink[i] = label(r, FONT_S, C_MUTED, "");
        lv_obj_align(s_plink[i], LV_ALIGN_RIGHT_MID, 0, 0);
        settings_printer_t *p = &s_edit.printer[i];
        edit_row(sec, "名称（收不到打印机广播时显示）", p->name, sizeof(p->name), false, s_printer_save);
        edit_row(sec, "IP 地址", p->host, sizeof(p->host), false, s_printer_save);
        edit_row(sec, "序列号 (SN)", p->serial, sizeof(p->serial), false, s_printer_save);
        edit_row(sec, "局域网访问码", p->access_code, sizeof(p->access_code), false, s_printer_save);
    }
    lv_obj_t *note = label(sec, FONT_S, C_MUTED, "访问码在打印机屏幕的设置 → WLAN 里查看，开关 LAN Only 后可能会变。");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(note, 12, 0);
    lv_obj_move_to_index(lv_obj_get_parent(s_printer_save), -1);
}

static void update_net(void)
{
    char buf[80], ip[20];
    switch (net_state()) {
    case NET_NO_CONFIG: snprintf(buf, sizeof(buf), "未配置"); break;
    case NET_CONNECTING: snprintf(buf, sizeof(buf), "连接中…"); break;
    case NET_DISCONNECTED: snprintf(buf, sizeof(buf), "已断开，正在重连"); break;
    case NET_CONNECTED:
        net_ip_str(ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "已连接  %s  信号 %d dBm", ip, net_rssi());
        break;
    default: buf[0] = 0;
    }
    set_text(s_wifi_info, buf);
    for (int i = 0; i < printers_count() && i < SETTINGS_PRINTERS; i++) {
        printer_snapshot_t s;
        if (!printer_get(i, &s)) continue;
        char nm[48];
        snprintf(buf, sizeof(buf), "%s · %s", printer_display_name(i, nm, sizeof(nm)), printer_link_name(s.link));
        set_text(s_plink[i], buf);
    }
}

// ---------- 关于 ----------
static void do_restart(void) { sys_restart(); }

static void do_reset(void)
{
    settings_reset();
    sys_restart();
}

static void restart_clicked(lv_event_t *e) { confirm("重启", "确定要重启屏幕吗？", "重启", C_GREEN, do_restart); }

static void reset_clicked(lv_event_t *e)
{
    confirm("恢复默认设置", "清除屏幕上改过的所有设置（含 Wi-Fi 和打印机），恢复为固件里的默认值并重启。", "恢复并重启", C_RED,
            do_reset);
}

static void build_about(lv_obj_t *sec)
{
    section_title(sec, "关于");
    lv_obj_t *l = label(sec, FONT_L, C_TEXT, "Bambu-Lan");
    lv_obj_set_style_pad_bottom(l, 4, 0);
    l = label(sec, FONT_S, C_MUTED, "拓竹打印机局域网状态 / 控制屏 · ESP32-P4");
    lv_obj_set_style_pad_bottom(l, 12, 0);
    s_about = label(sec, FONT_S, C_TEXT, "");
    lv_obj_set_style_text_line_space(s_about, 8, 0);
    lv_obj_t *r = plain(sec);
    lv_obj_set_size(r, LV_PCT(100), 100);
    lv_obj_t *b = button(r, LV_SYMBOL_REFRESH " 重启", C_CARD2, 160, 56, restart_clicked, NULL);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
    b = button(r, "恢复默认设置", C_RED, 200, 56, reset_clicked, NULL);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 180, 0);
}

static void update_about(void)
{
    static char buf[400];
    sys_info(buf, sizeof(buf));
    set_text(s_about, buf);
}

// ---------- 页面 ----------
static void show_section(int i)
{
    s_cur_sec = i;
    for (int k = 0; k < NAV_N; k++) {
        bool on = k == i;
        lv_obj_set_style_bg_opa(s_nav[k], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (on) lv_obj_remove_flag(s_sec[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sec[k], LV_OBJ_FLAG_HIDDEN);
    }
    if (i == SEC_TIME) update_time();
    if (i == SEC_WIFI || i == SEC_PRINTER) update_net();
    if (i == SEC_ABOUT) update_about();
}

static void nav_clicked(lv_event_t *e) { show_section((int)(intptr_t)lv_event_get_user_data(e)); }

static void back_clicked(lv_event_t *e)
{
    edit_close(false);
    lv_screen_load_anim(s_back_to, LV_SCR_LOAD_ANIM_MOVE_RIGHT, ui_page_anim_ms(), 0, false);
}

void ui_settings_build(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_edit = *settings_get();

    ui_header_band(s_scr);
    lv_obj_t *back = button(s_scr, LV_SYMBOL_LEFT " 返回", C_CARD, 120, 44, back_clicked, NULL);
    lv_obj_set_pos(back, 12, 10);
    lv_obj_t *t = label(s_scr, FONT_L, C_TEXT, "设置");
    lv_obj_set_pos(t, 150, 14);

    lv_obj_t *nav = plain(s_scr);
    lv_obj_set_pos(nav, 12, 64);
    lv_obj_set_size(nav, 220, 524);
    lv_obj_set_style_bg_color(nav, C_CARD, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nav, 16, 0);
    lv_obj_set_style_pad_all(nav, 10, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nav, 6, 0);

    lv_obj_t *content = plain(s_scr);
    lv_obj_set_pos(content, 244, 64);
    lv_obj_set_size(content, 768, 524);
    lv_obj_set_style_bg_color(content, C_CARD, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(content, 16, 0);
    lv_obj_set_style_clip_corner(content, false, 0);

    for (int i = 0; i < NAV_N; i++) {
        lv_obj_t *b = lv_button_create(nav);
        lv_obj_set_size(b, LV_PCT(100), 60);
        lv_obj_set_style_bg_color(b, C_CARD2, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *l = label(b, FONT_S, C_TEXT, s_nav_txt[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_add_event_cb(b, nav_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_nav[i] = b;

        lv_obj_t *sec = lv_obj_create(content);
        lv_obj_remove_style_all(sec);
        lv_obj_set_size(sec, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_hor(sec, 24, 0);
        lv_obj_set_style_pad_ver(sec, 16, 0);
        lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(sec, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(sec, LV_SCROLLBAR_MODE_AUTO);
        s_sec[i] = sec;
    }
    build_display(s_sec[SEC_DISPLAY]);
    build_time(s_sec[SEC_TIME]);
    build_wifi(s_sec[SEC_WIFI]);
    build_printers(s_sec[SEC_PRINTER]);
    build_about(s_sec[SEC_ABOUT]);
    show_section(SEC_DISPLAY);

    // 息屏时盖在最上层，吃掉唤醒的那一下点击
    s_blocker = plain(lv_layer_top());
    lv_obj_set_size(s_blocker, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_blocker, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
}

void ui_settings_open(lv_obj_t *back_to)
{
    s_back_to = back_to;
    s_edit = *settings_get();
    for (int i = 0; i < s_ntargets; i++) {
        show_value(&s_targets[i]);
        lv_obj_add_state(s_targets[i].save_btn, LV_STATE_DISABLED);
    }
    show_section(s_cur_sec);
    lv_screen_load_anim(s_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, ui_page_anim_ms(), 0, false);
}

uint32_t ui_page_anim_ms(void) { return settings_get()->page_anim ? 200 : 0; }

// ---------- 自动息屏 ----------
static void wake(void)
{
    if (!s_asleep) return;
    s_asleep = false;
    lv_obj_add_flag(s_blocker, LV_OBJ_FLAG_HIDDEN);
    sys_set_backlight(settings_get()->brightness);
    lv_display_trigger_activity(NULL);
}

static void blocker_pressed(lv_event_t *e) { wake(); }

void ui_settings_tick(bool any_printing, bool wake_event)
{
    static int tick;
    static bool cb_added;
    if (!cb_added) {
        lv_obj_add_event_cb(s_blocker, blocker_pressed, LV_EVENT_PRESSED, NULL);
        cb_added = true;
    }
    tick++;
    if (lv_screen_active() == s_scr) {
        if (s_cur_sec == SEC_TIME) update_time();
        if ((s_cur_sec == SEC_WIFI || s_cur_sec == SEC_PRINTER) && tick % 8 == 0) update_net();
        if (s_cur_sec == SEC_ABOUT && tick % 4 == 0) update_about();
    }

    const settings_t *st = settings_get();
    if (wake_event) {
        lv_display_trigger_activity(NULL);
        wake();
        return;
    }
    if (s_asleep || st->sleep_min == 0) return;
    if (any_printing && !st->sleep_when_printing) return;
    if (lv_display_get_inactive_time(NULL) < (uint32_t)st->sleep_min * 60000u) return;
    s_asleep = true;
    edit_close(false);
    lv_obj_remove_flag(s_blocker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_blocker);
    sys_set_backlight(st->sleep_level);
}

void ui_settings_debug_next(void)
{
    if (lv_screen_active() != s_scr) {
        s_cur_sec = 0;
        ui_settings_open(lv_screen_active());
    } else {
        show_section((s_cur_sec + 1) % NAV_N);
    }
}
