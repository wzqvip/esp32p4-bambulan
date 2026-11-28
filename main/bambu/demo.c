// 演示机器：每台一个固定场景，打印进度按时间推进（每 1% 约 15 秒，静止时不会频繁刷新界面）。
// 控制命令直接改本地状态：暂停/继续/停止、速度、灯、温度、风扇都会在界面上生效。
#include "demo.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bool init;
    bbl_gcode_state_t gs;
    int64_t last_ms;
    int64_t progress_ms;   // 已打印的“标准速度”时长
    int64_t finish_ms;     // 打完的时间
    int nozzle_t, bed_t, fan, speed;
    bool light;
} demo_state_t;

typedef struct {
    const char *name, *task;
    bbl_gcode_state_t gs;
    int start_pct;
    int total_min;          // 标准速度下整件要多久
    int total_layers;
    int nozzle, bed;
    int tray_now;           // 0..3 AMS；254 外挂；255 无
    bool ams;
    uint32_t colors[4];
    const char *types[4];
    int remain[4];
    uint32_t vt_color;
    const char *vt_type;
} demo_scene_t;

static const demo_scene_t s_scenes[DEMO_MAX] = {
    {"演示 A1 一号", "演示：手机支架 v3", BBL_GS_RUNNING, 37, 25, 180, 220, 65, 1, true,
     {0xF4F4F4, 0x2E8B57, 0x1E1E1E, 0xE0A030}, {"PLA", "PLA", "PETG", "PLA"}, {80, 46, 92, 15}, 0, NULL},
    {"演示 A1 mini", "演示：齿轮收纳盒", BBL_GS_PAUSE, 63, 40, 240, 220, 60, 2, true,
     {0xC0392B, 0x3366CC, 0xFFD700, 0x9B59B6}, {"PLA", "PLA", "PLA", "TPU"}, {55, 70, 12, 100}, 0, NULL},
    {"演示 P1S", "演示：城堡模型", BBL_GS_FINISH, 100, 90, 420, 30, 30, 255, true,
     {0xFFFFFF, 0x000000, 0x808080, 0}, {"PLA", "PLA", "PLA", ""}, {33, 61, 88, -1}, 0, NULL},
    {"演示 X1C", "演示：无人机机架", BBL_GS_RUNNING, 12, 60, 300, 260, 90, 254, false,
     {0}, {""}, {0}, 0x2B2B2B, "PETG-CF"},
};

static demo_state_t s_st[DEMO_MAX];

static int speed_mag(int lvl) { return lvl == 1 ? 50 : lvl == 3 ? 124 : lvl == 4 ? 166 : 100; }

static void step(int d, int64_t now_ms)
{
    const demo_scene_t *sc = &s_scenes[d];
    demo_state_t *s = &s_st[d];
    if (!s->init) {
        s->init = true;
        s->gs = sc->gs;
        s->progress_ms = (int64_t)sc->total_min * 60000 * sc->start_pct / 100;
        s->nozzle_t = sc->gs == BBL_GS_RUNNING || sc->gs == BBL_GS_PAUSE ? sc->nozzle : 0;
        s->bed_t = sc->gs == BBL_GS_RUNNING || sc->gs == BBL_GS_PAUSE ? sc->bed : 0;
        s->fan = sc->gs == BBL_GS_RUNNING ? 80 : 0;
        s->speed = 2;
        s->light = true;
        s->last_ms = now_ms;
    }
    int64_t dt = now_ms - s->last_ms;
    s->last_ms = now_ms;
    if (s->gs == BBL_GS_FINISH && sc->gs != BBL_GS_FINISH && now_ms - s->finish_ms > 60000) {
        s->gs = BBL_GS_RUNNING;
        s->progress_ms = 0;
        s->nozzle_t = sc->nozzle;
        s->bed_t = sc->bed;
        s->fan = 80;
    }
    if (s->gs != BBL_GS_RUNNING) return;
    s->progress_ms += dt * speed_mag(s->speed) / 100;
    int64_t total = (int64_t)sc->total_min * 60000;
    if (s->progress_ms >= total) {   // 打完了：停 1 分钟后从头再来，演示可以一直看
        s->progress_ms = total;
        s->gs = BBL_GS_FINISH;
        s->finish_ms = now_ms;
        s->nozzle_t = s->bed_t = 0;
        s->fan = 0;
    }
}

void demo_get(int d, int64_t now_ms, int64_t unix_now, printer_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (d < 0 || d >= DEMO_MAX) return;
    step(d, now_ms);
    const demo_scene_t *sc = &s_scenes[d];
    const demo_state_t *s = &s_st[d];
    bbl_status_t *st = &out->st;
    int64_t total = (int64_t)sc->total_min * 60000;

    out->link = PRINTER_LINK_ONLINE;
    out->rx_count = 1;
    out->last_rx_us = now_ms * 1000;
    snprintf(out->dev_name, sizeof(out->dev_name), "%s", sc->name);

    st->valid = true;
    st->gcode_state = s->gs;
    bool job = s->gs == BBL_GS_RUNNING || s->gs == BBL_GS_PAUSE || s->gs == BBL_GS_FINISH;
    if (job) {
        st->percent = (int)(s->progress_ms * 100 / total);
        st->remaining_min = (int)((total - s->progress_ms) / 60000 * 100 / speed_mag(s->speed));
        st->total_layers = sc->total_layers;
        st->layer = st->percent * sc->total_layers / 100;
        if (st->layer < 1) st->layer = 1;
        snprintf(st->subtask, sizeof(st->subtask), "%s", sc->task);
        snprintf(st->gcode_file, sizeof(st->gcode_file), "%s.gcode", sc->task);
        // 开始时间按分钟取整，免得每秒都变
        if (unix_now > 0) st->start_time = (unix_now - s->progress_ms / 1000) / 60 * 60;
    }
    // 温度直接等于目标值（不加噪声：数值一直跳会让界面一直重画）
    st->nozzle = st->nozzle_target = (float)s->nozzle_t;
    st->bed = st->bed_target = (float)s->bed_t;
    if (s->nozzle_t == 0) st->nozzle = 28;
    if (s->bed_t == 0) st->bed = 27;
    st->part_fan_pct = s->fan;
    st->speed_level = s->speed;
    st->speed_mag = speed_mag(s->speed);
    st->has_light = true;
    st->light_on = s->light;
    st->wifi_dbm = -48 - d * 6;

    st->ams_trays = sc->ams ? 4 : 0;
    for (int i = 0; i < 4 && sc->ams; i++) {
        bbl_tray_t *t = &st->tray[i];
        t->present = sc->types[i] && sc->types[i][0];
        t->loaded = t->present;
        t->color = sc->colors[i];
        snprintf(t->type, sizeof(t->type), "%s", t->present ? sc->types[i] : "");
        t->remain = t->present ? sc->remain[i] : -1;
    }
    st->tray_now = job && s->gs != BBL_GS_FINISH ? sc->tray_now : 255;
    if (sc->vt_type) {
        st->vt.present = st->vt.loaded = true;
        st->vt.color = sc->vt_color;
        snprintf(st->vt.type, sizeof(st->vt.type), "%s", sc->vt_type);
        st->vt.remain = -1;
    }
}

const char *demo_name(int d) { return d >= 0 && d < DEMO_MAX ? s_scenes[d].name : "-"; }

void demo_cmd(int d, demo_cmd_t cmd, int val)
{
    if (d < 0 || d >= DEMO_MAX) return;
    demo_state_t *s = &s_st[d];
    const demo_scene_t *sc = &s_scenes[d];
    switch (cmd) {
    case DEMO_CMD_PAUSE:
        if (s->gs == BBL_GS_RUNNING) s->gs = BBL_GS_PAUSE;
        break;
    case DEMO_CMD_RESUME:
        if (s->gs == BBL_GS_PAUSE) s->gs = BBL_GS_RUNNING;
        break;
    case DEMO_CMD_STOP:
        s->gs = BBL_GS_IDLE;
        s->nozzle_t = s->bed_t = s->fan = 0;
        s->progress_ms = 0;
        break;
    case DEMO_CMD_SPEED: s->speed = val < 1 ? 1 : val > 4 ? 4 : val; break;
    case DEMO_CMD_LIGHT: s->light = val != 0; break;
    case DEMO_CMD_NOZZLE: s->nozzle_t = val; break;
    case DEMO_CMD_BED: s->bed_t = val; break;
    case DEMO_CMD_FAN: s->fan = val; break;
    }
    // 停止后的空闲机器：再“继续”就从头开始一件新的，方便演示
    if (cmd == DEMO_CMD_RESUME && s->gs == BBL_GS_IDLE) {
        s->gs = BBL_GS_RUNNING;
        s->nozzle_t = sc->nozzle;
        s->bed_t = sc->bed;
        s->fan = 80;
    }
}
