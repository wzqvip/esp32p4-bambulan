// 界面：双机概览页 + 单机详情/控制页（LVGL 9，1024x600）
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"

#include "net.h"
#include "printer.h"

// ---------- 配色 ----------
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

#define FONT_S (&ui_font_cn_20)
#define FONT_L (&ui_font_cn_28)
#define FONT_NUM (&ui_font_num_56)

#define EDIT_HOLD_MS 4000   // 用户刚改过的控件，这段时间内不被打印机回报覆盖
#define TEMP_SEND_DELAY_MS 900

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// 用户在 EDIT_HOLD_MS 内改过这个控件（edit_ms 为 0 表示没改过）
static bool recently_edited(int64_t edit_ms, int64_t t) { return edit_ms != 0 && t - edit_ms < EDIT_HOLD_MS; }

// ---------- 概览卡片 ----------
typedef struct {
    lv_obj_t *card, *name, *state, *link, *file, *arc, *pct, *remain, *eta, *layer, *speed, *nozzle, *bed, *note;
} card_t;

// ---------- 详情页 ----------
typedef struct {
    int value;
    int64_t edit_ms;
    bool pending;
} edit_val_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *title, *state, *link;
    lv_obj_t *arc, *pct, *file, *remain, *eta, *layer, *nozzle, *bed, *fan, *speed_lbl, *note;
    lv_obj_t *btn_pause, *btn_pause_lbl, *btn_stop;
    lv_obj_t *speed_btns;
    lv_obj_t *nozzle_val, *bed_val;
    lv_obj_t *fan_slider, *fan_val;
    lv_obj_t *light_sw;
    lv_obj_t *status;
    lv_obj_t *ctrl_widgets[16];
    int ctrl_count;
    edit_val_t nozzle_edit, bed_edit;
    int64_t fan_edit_ms, light_edit_ms, speed_edit_ms;
    uint32_t last_result_seq;
} detail_t;

static lv_obj_t *s_overview;
static lv_obj_t *s_clock, *s_wifi;
static card_t s_cards[PRINTER_MAX];
static detail_t s_d;
static int s_detail_idx = -1;
static lv_obj_t *s_confirm;

// ---------- 小工具 ----------
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 16, 0);
    lv_obj_set_style_pad_all(o, 20, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_badge(lv_obj_t *parent)
{
    lv_obj_t *b = mk_label(parent, FONT_S, C_TEXT, "");
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    return b;
}

static lv_obj_t *mk_arc(lv_obj_t *parent, int size, int width)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, C_GREEN, LV_PART_INDICATOR);
    return a;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, lv_color_t bg, int w, int h, lv_event_cb_t cb, void *ud,
                        lv_obj_t **out_lbl)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_CARD2, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, C_MUTED, LV_STATE_DISABLED);
    lv_obj_t *l = mk_label(b, FONT_S, C_TEXT, txt);
    lv_obj_set_style_text_color(l, C_MUTED, LV_STATE_DISABLED);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (out_lbl) *out_lbl = l;
    return b;
}

static void fmt_duration(char *buf, size_t n, int minutes)
{
    if (minutes <= 0) snprintf(buf, n, "-");
    else if (minutes < 60) snprintf(buf, n, "%d分钟", minutes);
    else snprintf(buf, n, "%d小时%02d分", minutes / 60, minutes % 60);
}

static void fmt_eta(char *buf, size_t n, const bbl_status_t *st)
{
    if (!bbl_state_is_active(st->gcode_state) || st->remaining_min <= 0 || !net_time_valid()) {
        snprintf(buf, n, "-");
        return;
    }
    time_t now = time(NULL), end = now + st->remaining_min * 60;
    struct tm tn, te;
    localtime_r(&now, &tn);
    localtime_r(&end, &te);
    int days = (te.tm_year - tn.tm_year) * 366 + (te.tm_yday - tn.tm_yday);
    if (days == 0) snprintf(buf, n, "%02d:%02d", te.tm_hour, te.tm_min);
    else if (days == 1) snprintf(buf, n, "明天 %02d:%02d", te.tm_hour, te.tm_min);
    else snprintf(buf, n, "%d/%d %02d:%02d", te.tm_mon + 1, te.tm_mday, te.tm_hour, te.tm_min);
}

static const char *speed_name(int lvl)
{
    switch (lvl) {
    case 1: return "静音";
    case 2: return "标准";
    case 3: return "运动";
    case 4: return "狂暴";
    default: return "-";
    }
}

static lv_color_t state_color(bbl_gcode_state_t st)
{
    switch (st) {
    case BBL_GS_RUNNING: return C_GREEN;
    case BBL_GS_PAUSE: return C_WARN;
    case BBL_GS_PREPARE:
    case BBL_GS_SLICING: return C_BLUE;
    case BBL_GS_FINISH: return C_TEAL;
    case BBL_GS_FAILED: return C_RED;
    default: return C_CARD2;
    }
}

static void set_badge(lv_obj_t *badge, const printer_snapshot_t *s)
{
    if (s->link == PRINTER_LINK_ONLINE && s->st.valid) {
        lv_label_set_text(badge, bbl_gcode_state_name(s->st.gcode_state));
        lv_obj_set_style_bg_color(badge, state_color(s->st.gcode_state), 0);
    } else {
        lv_label_set_text(badge, printer_link_name(s->link));
        bool bad = s->link == PRINTER_LINK_AUTH_FAILED || s->link == PRINTER_LINK_TLS_FAILED;
        lv_obj_set_style_bg_color(badge, bad ? C_RED : C_CARD2, 0);
    }
}

static const char *task_name(const bbl_status_t *st)
{
    if (st->subtask[0]) return st->subtask;
    if (st->gcode_file[0]) return st->gcode_file;
    return "无打印任务";
}

// 卡片/详情底部的提示行
static void fmt_note(char *buf, size_t n, const printer_snapshot_t *s, lv_color_t *color)
{
    *color = C_MUTED;
    buf[0] = '\0';
    switch (s->link) {
    case PRINTER_LINK_NOT_CONFIGURED:
        snprintf(buf, n, "请在 menuconfig 填写 IP、序列号和访问码");
        *color = C_WARN;
        return;
    case PRINTER_LINK_AUTH_FAILED:
        snprintf(buf, n, "访问码错误，请核对打印机屏幕上的局域网访问码");
        *color = C_RED;
        return;
    case PRINTER_LINK_TLS_FAILED:
        snprintf(buf, n, "TLS 校验失败，可在 menuconfig 关闭证书校验再试");
        *color = C_RED;
        return;
    case PRINTER_LINK_OFFLINE:
        snprintf(buf, n, "连不上打印机，正在重连…");
        *color = C_WARN;
        return;
    default:
        break;
    }
    if (s->st.print_error) {
        snprintf(buf, n, "打印机报错 0x%08lX", (unsigned long)s->st.print_error);
        *color = C_RED;
    } else if (s->st.hms_count > 0) {
        snprintf(buf, n, "有 %d 条 HMS 提示，请看打印机屏幕", s->st.hms_count);
        *color = C_WARN;
    } else if (s->st.signature_required) {
        snprintf(buf, n, "只读：未开开发者模式，控制命令会被拒绝");
        *color = C_WARN;
    }
}

// ================= 概览页 =================
static void show_detail(int idx);

static void card_clicked(lv_event_t *e)
{
    show_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_card(card_t *c, lv_obj_t *parent, int idx, int x)
{
    c->card = mk_panel(parent, x, 64, 494, 524);
    lv_obj_add_flag(c->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c->card, C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c->card, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    const printer_cfg_t *cfg = printer_cfg(idx);
    c->name = mk_label(c->card, FONT_L, C_TEXT, cfg ? cfg->name : "-");
    c->state = mk_badge(c->card);
    lv_obj_align(c->state, LV_ALIGN_TOP_RIGHT, 0, 2);

    c->link = mk_label(c->card, FONT_S, C_MUTED, "");
    lv_obj_set_pos(c->link, 0, 42);

    c->file = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_set_width(c->file, 454);
    lv_label_set_long_mode(c->file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(c->file, 0, 78);

    c->arc = mk_arc(c->card, 220, 18);
    lv_obj_align(c->arc, LV_ALIGN_TOP_MID, 0, 116);
    c->pct = mk_label(c->card, FONT_NUM, C_TEXT, "-");
    lv_obj_align_to(c->pct, c->arc, LV_ALIGN_CENTER, 0, 0);

    int y = 352;
    c->remain = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_set_pos(c->remain, 0, y);
    c->eta = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_align(c->eta, LV_ALIGN_TOP_RIGHT, 0, y);
    y += 36;
    c->layer = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_set_pos(c->layer, 0, y);
    c->speed = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_align(c->speed, LV_ALIGN_TOP_RIGHT, 0, y);
    y += 36;
    c->nozzle = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_set_pos(c->nozzle, 0, y);
    c->bed = mk_label(c->card, FONT_S, C_TEXT, "");
    lv_obj_align(c->bed, LV_ALIGN_TOP_RIGHT, 0, y);
    y += 40;
    c->note = mk_label(c->card, FONT_S, C_MUTED, "");
    lv_obj_set_width(c->note, 454);
    lv_label_set_long_mode(c->note, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(c->note, 0, y);
}

static void update_card(card_t *c, int idx, const printer_snapshot_t *s)
{
    const printer_cfg_t *cfg = printer_cfg(idx);
    char buf[128], t[48];
    const bbl_status_t *st = &s->st;
    bool has = s->link == PRINTER_LINK_ONLINE && st->valid;
    bool active = has && bbl_state_is_active(st->gcode_state);

    set_badge(c->state, s);
    snprintf(buf, sizeof(buf), "%s · %s", printer_link_name(s->link), cfg && cfg->host[0] ? cfg->host : "-");
    lv_label_set_text(c->link, buf);
    lv_label_set_text(c->file, has ? task_name(st) : "");

    int pct = has ? st->percent : 0;
    lv_arc_set_value(c->arc, pct);
    lv_obj_set_style_arc_color(c->arc, has ? state_color(st->gcode_state) : C_CARD2, LV_PART_INDICATOR);
    if (has) snprintf(buf, sizeof(buf), "%d%%", pct);
    else snprintf(buf, sizeof(buf), "-");
    lv_label_set_text(c->pct, buf);
    lv_obj_align_to(c->pct, c->arc, LV_ALIGN_CENTER, 0, 0);

    fmt_duration(t, sizeof(t), active ? st->remaining_min : 0);
    snprintf(buf, sizeof(buf), "剩余 %s", t);
    lv_label_set_text(c->remain, buf);
    fmt_eta(t, sizeof(t), st);
    snprintf(buf, sizeof(buf), "预计完成 %s", has ? t : "-");
    lv_label_set_text(c->eta, buf);

    if (active && st->total_layers > 0) snprintf(buf, sizeof(buf), "层 %d / %d", st->layer, st->total_layers);
    else snprintf(buf, sizeof(buf), "层 -");
    lv_label_set_text(c->layer, buf);
    snprintf(buf, sizeof(buf), "速度 %s", has ? speed_name(st->speed_level) : "-");
    lv_label_set_text(c->speed, buf);

    if (has) snprintf(buf, sizeof(buf), "喷嘴 %.0f / %.0f°C", st->nozzle, st->nozzle_target);
    else snprintf(buf, sizeof(buf), "喷嘴 -");
    lv_label_set_text(c->nozzle, buf);
    if (has) snprintf(buf, sizeof(buf), "热床 %.0f / %.0f°C", st->bed, st->bed_target);
    else snprintf(buf, sizeof(buf), "热床 -");
    lv_label_set_text(c->bed, buf);

    lv_color_t col;
    fmt_note(buf, sizeof(buf), s, &col);
    lv_label_set_text(c->note, buf[0] ? buf : "点按查看详情和控制");
    lv_obj_set_style_text_color(c->note, buf[0] ? col : C_MUTED, 0);
}

static void build_topbar(lv_obj_t *scr, const char *title, lv_obj_t **clock, lv_obj_t **wifi)
{
    lv_obj_t *t = mk_label(scr, FONT_L, C_TEXT, title);
    lv_obj_set_pos(t, 20, 14);
    if (clock) {
        *clock = mk_label(scr, FONT_L, C_TEXT, "--:--");
        lv_obj_align(*clock, LV_ALIGN_TOP_RIGHT, -20, 14);
    }
    if (wifi) {
        *wifi = mk_label(scr, FONT_S, C_MUTED, "");
        lv_obj_align(*wifi, LV_ALIGN_TOP_RIGHT, -120, 20);
    }
}

static void build_overview(void)
{
    s_overview = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_overview, C_BG, 0);
    lv_obj_remove_flag(s_overview, LV_OBJ_FLAG_SCROLLABLE);
    build_topbar(s_overview, "拓竹打印机", &s_clock, &s_wifi);

    int n = printers_count();
    for (int i = 0; i < n; i++) {
        int x = n == 1 ? (1024 - 494) / 2 : 12 + i * 506;
        build_card(&s_cards[i], s_overview, i, x);
    }
}

// ================= 详情页 =================
static void back_clicked(lv_event_t *e)
{
    s_detail_idx = -1;
    lv_screen_load_anim(s_overview, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void set_status(const char *txt, lv_color_t color)
{
    lv_label_set_text(s_d.status, txt);
    lv_obj_set_style_text_color(s_d.status, color, 0);
}

static void report_send(esp_err_t err, const char *what)
{
    char buf[96];
    if (err == ESP_OK) {
        snprintf(buf, sizeof(buf), "已发送：%s", what);
        set_status(buf, C_MUTED);
    } else if (err == ESP_ERR_INVALID_STATE) {
        set_status("打印机未连接，命令没有发送", C_RED);
    } else {
        snprintf(buf, sizeof(buf), "发送失败：%s", what);
        set_status(buf, C_RED);
    }
}

static void pause_clicked(lv_event_t *e)
{
    printer_snapshot_t s;
    if (!printer_get(s_detail_idx, &s)) return;
    if (s.st.gcode_state == BBL_GS_PAUSE) report_send(printer_resume(s_detail_idx), "继续打印");
    else report_send(printer_pause(s_detail_idx), "暂停打印");
}

static void confirm_cb(lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);
    if (yes) report_send(printer_stop(s_detail_idx), "停止打印");
    if (s_confirm) {
        lv_msgbox_close(s_confirm);
        s_confirm = NULL;
    }
}

static void stop_clicked(lv_event_t *e)
{
    if (s_confirm) return;
    const printer_cfg_t *cfg = printer_cfg(s_detail_idx);
    char buf[96];
    snprintf(buf, sizeof(buf), "确定要停止 %s 的打印吗？停止后无法继续。", cfg ? cfg->name : "");
    s_confirm = lv_msgbox_create(NULL);
    lv_obj_set_style_text_font(s_confirm, FONT_S, 0);
    lv_obj_set_style_bg_color(s_confirm, C_CARD, 0);
    lv_obj_set_style_radius(s_confirm, 16, 0);
    lv_obj_set_style_pad_all(s_confirm, 12, 0);
    lv_obj_set_width(s_confirm, 600);
    lv_obj_t *title = lv_msgbox_add_title(s_confirm, "停止打印");
    lv_obj_set_style_text_font(title, FONT_L, 0);
    lv_obj_t *text = lv_msgbox_add_text(s_confirm, buf);
    lv_obj_set_style_pad_ver(text, 16, 0);
    lv_obj_t *no = lv_msgbox_add_footer_button(s_confirm, "取消");
    lv_obj_set_size(no, 160, 56);
    lv_obj_set_style_bg_color(no, C_CARD2, 0);
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_confirm, "确认停止");
    lv_obj_set_size(yes, 160, 56);
    lv_obj_set_style_bg_color(yes, C_RED, 0);
    lv_obj_add_event_cb(yes, confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_set_height(lv_msgbox_get_footer(s_confirm), 80);
    lv_obj_set_style_bg_opa(lv_msgbox_get_header(s_confirm), LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(no, confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
}

static const char *s_speed_map[] = {"静音", "标准", "运动", "狂暴", ""};

static void speed_changed(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(s_d.speed_btns);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || id > 3) return;
    s_d.speed_edit_ms = now_ms();
    char what[32];
    snprintf(what, sizeof(what), "速度 %s", s_speed_map[id]);
    report_send(printer_set_speed(s_detail_idx, (int)id + 1), what);
}

typedef struct {
    bool bed;
    int step;   // 0 表示“关”（目标设为 0）
} temp_btn_t;

static const temp_btn_t s_temp_btns[] = {
    {false, -5}, {false, 5}, {false, 0},
    {true, -5}, {true, 5}, {true, 0},
};

static void temp_step(lv_event_t *e)
{
    const temp_btn_t *tb = lv_event_get_user_data(e);
    edit_val_t *ev = tb->bed ? &s_d.bed_edit : &s_d.nozzle_edit;
    int maxv = tb->bed ? 100 : 300;
    ev->value = tb->step == 0 ? 0 : ev->value + tb->step;
    if (ev->value < 0) ev->value = 0;
    if (ev->value > maxv) ev->value = maxv;
    ev->edit_ms = now_ms();
    ev->pending = true;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d°C", ev->value);
    lv_label_set_text(tb->bed ? s_d.bed_val : s_d.nozzle_val, buf);
}

static void fan_released(lv_event_t *e)
{
    int v = lv_slider_get_value(s_d.fan_slider);
    s_d.fan_edit_ms = now_ms();
    char what[32];
    snprintf(what, sizeof(what), "风扇 %d%%", v);
    report_send(printer_set_part_fan(s_detail_idx, v), what);
}

static void fan_changed(lv_event_t *e)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(s_d.fan_slider));
    lv_label_set_text(s_d.fan_val, buf);
    s_d.fan_edit_ms = now_ms();
}

static void light_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_d.light_sw, LV_STATE_CHECKED);
    s_d.light_edit_ms = now_ms();
    report_send(printer_set_light(s_detail_idx, on), on ? "开灯" : "关灯");
}

static void add_ctrl(lv_obj_t *o)
{
    if (s_d.ctrl_count < (int)(sizeof(s_d.ctrl_widgets) / sizeof(s_d.ctrl_widgets[0]))) {
        s_d.ctrl_widgets[s_d.ctrl_count++] = o;
    }
}

static void build_temp_row(lv_obj_t *p, int y, const char *label, bool bed, lv_obj_t **val)
{
    const temp_btn_t *tb = &s_temp_btns[bed ? 3 : 0];
    lv_obj_t *l = mk_label(p, FONT_S, C_MUTED, label);
    lv_obj_set_pos(l, 0, y + 14);
    lv_obj_t *b = mk_btn(p, LV_SYMBOL_MINUS " 5", C_CARD2, 84, 52, temp_step, (void *)&tb[0], NULL);
    lv_obj_set_pos(b, 130, y);
    add_ctrl(b);
    *val = mk_label(p, FONT_L, C_TEXT, "-");
    lv_obj_set_width(*val, 120);
    lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(*val, 220, y + 8);
    b = mk_btn(p, LV_SYMBOL_PLUS " 5", C_CARD2, 84, 52, temp_step, (void *)&tb[1], NULL);
    lv_obj_set_pos(b, 346, y);
    add_ctrl(b);
    b = mk_btn(p, "关", C_CARD2, 70, 52, temp_step, (void *)&tb[2], NULL);
    lv_obj_set_pos(b, 438, y);
    add_ctrl(b);
}

static void build_detail(void)
{
    detail_t *d = &s_d;
    d->scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(d->scr, C_BG, 0);
    lv_obj_remove_flag(d->scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = mk_btn(d->scr, LV_SYMBOL_LEFT " 返回", C_CARD, 120, 44, back_clicked, NULL, NULL);
    lv_obj_set_pos(back, 12, 10);
    d->title = mk_label(d->scr, FONT_L, C_TEXT, "");
    lv_obj_set_pos(d->title, 150, 14);
    d->state = mk_badge(d->scr);
    lv_obj_align(d->state, LV_ALIGN_TOP_RIGHT, -20, 16);
    d->link = mk_label(d->scr, FONT_S, C_MUTED, "");
    lv_obj_align(d->link, LV_ALIGN_TOP_RIGHT, -140, 20);

    // 左：状态
    lv_obj_t *l = mk_panel(d->scr, 12, 64, 440, 524);
    d->arc = mk_arc(l, 190, 16);
    lv_obj_align(d->arc, LV_ALIGN_TOP_MID, 0, 0);
    d->pct = mk_label(l, FONT_NUM, C_TEXT, "-");
    lv_obj_align_to(d->pct, d->arc, LV_ALIGN_CENTER, 0, 0);
    d->file = mk_label(l, FONT_S, C_TEXT, "");
    lv_obj_set_width(d->file, 400);
    lv_obj_set_style_text_align(d->file, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(d->file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(d->file, 0, 190);
    lv_obj_t **rows[] = {&d->remain, &d->eta, &d->layer, &d->nozzle, &d->bed, &d->fan, &d->speed_lbl};
    for (int i = 0; i < 7; i++) {
        *rows[i] = mk_label(l, FONT_S, C_TEXT, "");
        lv_obj_set_pos(*rows[i], 0, 232 + i * 32);
    }
    d->note = mk_label(l, FONT_S, C_MUTED, "");
    lv_obj_set_width(d->note, 400);
    lv_label_set_long_mode(d->note, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d->note, 0, 232 + 7 * 32 + 4);

    // 右：控制
    lv_obj_t *r = mk_panel(d->scr, 464, 64, 548, 524);
    d->btn_pause = mk_btn(r, LV_SYMBOL_PAUSE " 暂停", C_BLUE, 248, 64, pause_clicked, NULL, &d->btn_pause_lbl);
    lv_obj_set_pos(d->btn_pause, 0, 0);
    add_ctrl(d->btn_pause);
    d->btn_stop = mk_btn(r, LV_SYMBOL_STOP " 停止", C_RED, 248, 64, stop_clicked, NULL, NULL);
    lv_obj_set_pos(d->btn_stop, 260, 0);
    add_ctrl(d->btn_stop);

    lv_obj_t *sl = mk_label(r, FONT_S, C_MUTED, "打印速度");
    lv_obj_set_pos(sl, 0, 100);
    d->speed_btns = lv_buttonmatrix_create(r);
    lv_buttonmatrix_set_map(d->speed_btns, s_speed_map);
    lv_buttonmatrix_set_button_ctrl_all(d->speed_btns, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(d->speed_btns, true);
    lv_obj_set_size(d->speed_btns, 378, 56);
    lv_obj_set_pos(d->speed_btns, 130, 84);
    lv_obj_set_style_text_font(d->speed_btns, FONT_S, 0);
    lv_obj_set_style_bg_opa(d->speed_btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d->speed_btns, 0, 0);
    lv_obj_set_style_pad_all(d->speed_btns, 0, 0);
    lv_obj_set_style_pad_column(d->speed_btns, 6, 0);
    lv_obj_set_style_bg_color(d->speed_btns, C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(d->speed_btns, C_GREEN, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(d->speed_btns, C_TEXT, LV_PART_ITEMS);
    lv_obj_add_event_cb(d->speed_btns, speed_changed, LV_EVENT_VALUE_CHANGED, NULL);
    add_ctrl(d->speed_btns);

    build_temp_row(r, 158, "喷嘴目标", false, &d->nozzle_val);
    build_temp_row(r, 228, "热床目标", true, &d->bed_val);

    lv_obj_t *fl = mk_label(r, FONT_S, C_MUTED, "冷却风扇");
    lv_obj_set_pos(fl, 0, 316);
    d->fan_slider = lv_slider_create(r);
    lv_slider_set_range(d->fan_slider, 0, 100);
    lv_obj_set_size(d->fan_slider, 280, 16);
    lv_obj_set_pos(d->fan_slider, 140, 322);
    lv_obj_set_ext_click_area(d->fan_slider, 20);
    lv_obj_set_style_bg_color(d->fan_slider, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(d->fan_slider, C_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(d->fan_slider, fan_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(d->fan_slider, fan_released, LV_EVENT_RELEASED, NULL);
    add_ctrl(d->fan_slider);
    d->fan_val = mk_label(r, FONT_S, C_TEXT, "-");
    lv_obj_set_pos(d->fan_val, 448, 316);

    lv_obj_t *ll = mk_label(r, FONT_S, C_MUTED, "灯光");
    lv_obj_set_pos(ll, 0, 382);
    d->light_sw = lv_switch_create(r);
    lv_obj_set_size(d->light_sw, 80, 40);
    lv_obj_set_pos(d->light_sw, 140, 374);
    lv_obj_set_style_bg_color(d->light_sw, C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(d->light_sw, light_changed, LV_EVENT_VALUE_CHANGED, NULL);
    add_ctrl(d->light_sw);

    d->status = mk_label(r, FONT_S, C_MUTED, "");
    lv_obj_set_width(d->status, 508);
    lv_label_set_long_mode(d->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d->status, 0, 432);
}

static void show_detail(int idx)
{
    const printer_cfg_t *cfg = printer_cfg(idx);
    if (!cfg) return;
    s_detail_idx = idx;
    lv_label_set_text(s_d.title, cfg->name);
    set_status("", C_MUTED);
    s_d.nozzle_edit.edit_ms = s_d.bed_edit.edit_ms = 0;
    s_d.nozzle_edit.pending = s_d.bed_edit.pending = false;
    s_d.fan_edit_ms = s_d.light_edit_ms = s_d.speed_edit_ms = 0;
    printer_snapshot_t s;
    if (printer_get(idx, &s)) s_d.last_result_seq = s.st.result_seq;
    lv_screen_load_anim(s_d.scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static const char *cmd_name(const char *cmd)
{
    if (!strcmp(cmd, "pause")) return "暂停";
    if (!strcmp(cmd, "resume")) return "继续";
    if (!strcmp(cmd, "stop")) return "停止";
    if (!strcmp(cmd, "print_speed")) return "调速";
    if (!strcmp(cmd, "ledctrl")) return "灯光";
    if (!strcmp(cmd, "gcode_line")) return "温度/风扇";
    if (!strcmp(cmd, "pushall")) return "刷新状态";
    return cmd;
}

static void set_enabled(lv_obj_t *o, bool en)
{
    if (en) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

static void flush_temp_edit(edit_val_t *ev, bool bed, int64_t t)
{
    if (!ev->pending || t - ev->edit_ms < TEMP_SEND_DELAY_MS) return;
    ev->pending = false;
    ev->edit_ms = t;   // 发送后再保持一会儿，等打印机回报新目标
    char what[32];
    snprintf(what, sizeof(what), "%s %d°C", bed ? "热床" : "喷嘴", ev->value);
    report_send(bed ? printer_set_bed_temp(s_detail_idx, ev->value) : printer_set_nozzle_temp(s_detail_idx, ev->value),
                what);
}

static void update_detail(const printer_snapshot_t *s)
{
    detail_t *d = &s_d;
    const bbl_status_t *st = &s->st;
    const printer_cfg_t *cfg = printer_cfg(s_detail_idx);
    bool has = s->link == PRINTER_LINK_ONLINE && st->valid;
    bool active = has && bbl_state_is_active(st->gcode_state);
    char buf[128], t[48];
    int64_t tnow = now_ms();

    set_badge(d->state, s);
    snprintf(buf, sizeof(buf), "%s · %s", printer_link_name(s->link), cfg->host[0] ? cfg->host : "-");
    lv_label_set_text(d->link, buf);
    lv_obj_align_to(d->link, d->state, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    lv_arc_set_value(d->arc, has ? st->percent : 0);
    lv_obj_set_style_arc_color(d->arc, has ? state_color(st->gcode_state) : C_CARD2, LV_PART_INDICATOR);
    if (has) snprintf(buf, sizeof(buf), "%d%%", st->percent);
    else snprintf(buf, sizeof(buf), "-");
    lv_label_set_text(d->pct, buf);
    lv_obj_align_to(d->pct, d->arc, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(d->file, has ? task_name(st) : "");

    fmt_duration(t, sizeof(t), active ? st->remaining_min : 0);
    snprintf(buf, sizeof(buf), "剩余时间　%s", t);
    lv_label_set_text(d->remain, buf);
    fmt_eta(t, sizeof(t), st);
    snprintf(buf, sizeof(buf), "预计完成　%s", has ? t : "-");
    lv_label_set_text(d->eta, buf);
    if (active && st->total_layers > 0) snprintf(buf, sizeof(buf), "当前层　　%d / %d", st->layer, st->total_layers);
    else snprintf(buf, sizeof(buf), "当前层　　-");
    lv_label_set_text(d->layer, buf);
    if (has) snprintf(buf, sizeof(buf), "喷嘴温度　%.0f / %.0f°C", st->nozzle, st->nozzle_target);
    else snprintf(buf, sizeof(buf), "喷嘴温度　-");
    lv_label_set_text(d->nozzle, buf);
    if (has) snprintf(buf, sizeof(buf), "热床温度　%.0f / %.0f°C", st->bed, st->bed_target);
    else snprintf(buf, sizeof(buf), "热床温度　-");
    lv_label_set_text(d->bed, buf);
    if (has) snprintf(buf, sizeof(buf), "冷却风扇　%d%%", st->part_fan_pct);
    else snprintf(buf, sizeof(buf), "冷却风扇　-");
    lv_label_set_text(d->fan, buf);
    snprintf(buf, sizeof(buf), "打印速度　%s (%d%%)", has ? speed_name(st->speed_level) : "-", has ? st->speed_mag : 0);
    lv_label_set_text(d->speed_lbl, buf);

    lv_color_t col;
    fmt_note(buf, sizeof(buf), s, &col);
    lv_label_set_text(d->note, buf);
    lv_obj_set_style_text_color(d->note, col, 0);

    // 控件可用性
    for (int i = 0; i < d->ctrl_count; i++) set_enabled(d->ctrl_widgets[i], has);
    bool can_pause = has && (st->gcode_state == BBL_GS_RUNNING || st->gcode_state == BBL_GS_PAUSE ||
                             st->gcode_state == BBL_GS_PREPARE);
    set_enabled(d->btn_pause, can_pause);
    set_enabled(d->btn_stop, active);
    set_enabled(d->speed_btns, active);
    lv_label_set_text(d->btn_pause_lbl, st->gcode_state == BBL_GS_PAUSE ? LV_SYMBOL_PLAY " 继续" : LV_SYMBOL_PAUSE " 暂停");
    lv_obj_set_style_bg_color(d->btn_pause, st->gcode_state == BBL_GS_PAUSE ? C_GREEN : C_BLUE, 0);

    if (!has) return;

    // 回报值同步到控件（用户刚操作过的除外）
    flush_temp_edit(&d->nozzle_edit, false, tnow);
    flush_temp_edit(&d->bed_edit, true, tnow);
    if (!d->nozzle_edit.pending && !recently_edited(d->nozzle_edit.edit_ms, tnow)) {
        d->nozzle_edit.value = (int)(st->nozzle_target + 0.5f);
        snprintf(buf, sizeof(buf), "%d°C", d->nozzle_edit.value);
        lv_label_set_text(d->nozzle_val, buf);
    }
    if (!d->bed_edit.pending && !recently_edited(d->bed_edit.edit_ms, tnow)) {
        d->bed_edit.value = (int)(st->bed_target + 0.5f);
        snprintf(buf, sizeof(buf), "%d°C", d->bed_edit.value);
        lv_label_set_text(d->bed_val, buf);
    }
    if (!recently_edited(d->fan_edit_ms, tnow) && !lv_obj_has_state(d->fan_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(d->fan_slider, st->part_fan_pct, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d%%", st->part_fan_pct);
        lv_label_set_text(d->fan_val, buf);
    }
    if (!recently_edited(d->light_edit_ms, tnow)) {
        set_enabled(d->light_sw, st->has_light);
        if (st->light_on) lv_obj_add_state(d->light_sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(d->light_sw, LV_STATE_CHECKED);
    }
    if (!recently_edited(d->speed_edit_ms, tnow) && st->speed_level >= 1 && st->speed_level <= 4) {
        lv_buttonmatrix_set_button_ctrl(d->speed_btns, st->speed_level - 1, LV_BUTTONMATRIX_CTRL_CHECKED);
    }

    // 命令回执
    if (st->result_seq != d->last_result_seq) {
        d->last_result_seq = st->result_seq;
        if (strcmp(st->result_cmd, "pushall") != 0) {
            if (st->result_ok) {
                snprintf(buf, sizeof(buf), "打印机已执行：%s", cmd_name(st->result_cmd));
                set_status(buf, C_GREEN);
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "打印机拒绝了「%s」%s%s%s", cmd_name(st->result_cmd),
                         st->result_reason[0] ? "：" : "", st->result_reason,
                         st->signature_required ? "。需要在打印机上开启 LAN-only 和开发者模式" : "");
                set_status(msg, C_RED);
            }
        }
    }
}

// ================= 刷新 =================
static void refresh_cb(lv_timer_t *t)
{
    static int tick;
    static char wifi_txt[48];
    tick++;

    char buf[32];
    if (net_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text(s_clock, buf);
    }
    if (tick % 20 == 1) {   // Wi-Fi 信号查询要经 SDIO 问 C6，降低频率
        switch (net_state()) {
        case NET_NO_CONFIG: snprintf(wifi_txt, sizeof(wifi_txt), "未配置 Wi-Fi"); break;
        case NET_CONNECTING: snprintf(wifi_txt, sizeof(wifi_txt), LV_SYMBOL_WIFI " 连接中"); break;
        case NET_DISCONNECTED: snprintf(wifi_txt, sizeof(wifi_txt), LV_SYMBOL_WIFI " 已断开"); break;
        case NET_CONNECTED: snprintf(wifi_txt, sizeof(wifi_txt), LV_SYMBOL_WIFI " %d dBm", net_rssi()); break;
        }
        lv_label_set_text(s_wifi, wifi_txt);
        lv_obj_align_to(s_wifi, s_clock, LV_ALIGN_OUT_LEFT_MID, -20, 2);
    }

    printer_snapshot_t s;
    for (int i = 0; i < printers_count(); i++) {
        if (!printer_get(i, &s)) continue;
        update_card(&s_cards[i], i, &s);
        if (i == s_detail_idx) update_detail(&s);
    }
}

void ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, C_GREEN, C_BLUE, true, FONT_S);
    lv_display_set_theme(disp, th);

    build_overview();
    build_detail();
    lv_screen_load(s_overview);
    refresh_cb(NULL);
    lv_timer_create(refresh_cb, 250, NULL);
}
