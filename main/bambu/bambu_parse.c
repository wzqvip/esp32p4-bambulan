#include "bambu_parse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

// print.fun 里的这一位表示“需要签名的 MQTT 命令”（未开开发者模式）
#define BBL_FUN_MQTT_SIGNATURE_REQUIRED 0x20000000ULL
// print.flag3 里的这一位表示固件启用了授权控制
#define BBL_FLAG3_AUTH_CONTROL 0x10

void bbl_status_init(bbl_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->tray_now = 255;
    s->vt.remain = -1;
    s->speed_level = BBL_SPEED_STANDARD;
    s->speed_mag = 100;
}

static bbl_gcode_state_t parse_gcode_state(const char *v)
{
    if (!v) return BBL_GS_UNKNOWN;
    if (!strcmp(v, "IDLE")) return BBL_GS_IDLE;
    if (!strcmp(v, "PREPARE")) return BBL_GS_PREPARE;
    if (!strcmp(v, "RUNNING")) return BBL_GS_RUNNING;
    if (!strcmp(v, "PAUSE")) return BBL_GS_PAUSE;
    if (!strcmp(v, "FINISH")) return BBL_GS_FINISH;
    if (!strcmp(v, "FAILED")) return BBL_GS_FAILED;
    if (!strcmp(v, "SLICING")) return BBL_GS_SLICING;
    return BBL_GS_UNKNOWN;
}

const char *bbl_gcode_state_name(bbl_gcode_state_t st)
{
    switch (st) {
    case BBL_GS_IDLE: return "空闲";
    case BBL_GS_PREPARE: return "准备中";
    case BBL_GS_RUNNING: return "打印中";
    case BBL_GS_PAUSE: return "已暂停";
    case BBL_GS_FINISH: return "已完成";
    case BBL_GS_FAILED: return "失败";
    case BBL_GS_SLICING: return "切片中";
    default: return "未知";
    }
}

bool bbl_state_is_active(bbl_gcode_state_t st)
{
    return st == BBL_GS_RUNNING || st == BBL_GS_PAUSE || st == BBL_GS_PREPARE || st == BBL_GS_SLICING;
}

// 拓竹的数值字段有时是数字、有时是字符串（例如 "cooling_fan_speed":"15"）
static bool get_number(const cJSON *obj, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) return false;
    if (cJSON_IsNumber(it)) {
        *out = it->valuedouble;
        return true;
    }
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        char *end = NULL;
        double v = strtod(it->valuestring, &end);
        if (end == it->valuestring) return false;
        *out = v;
        return true;
    }
    if (cJSON_IsBool(it)) {
        *out = cJSON_IsTrue(it) ? 1 : 0;
        return true;
    }
    return false;
}

static bool upd_int(const cJSON *obj, const char *key, int *dst)
{
    double v;
    if (!get_number(obj, key, &v)) return false;
    int iv = (int)lround(v);
    if (*dst == iv) return false;
    *dst = iv;
    return true;
}

static bool upd_float(const cJSON *obj, const char *key, float *dst)
{
    double v;
    if (!get_number(obj, key, &v)) return false;
    if (fabsf(*dst - (float)v) < 0.01f) return false;
    *dst = (float)v;
    return true;
}

static bool upd_str(const cJSON *obj, const char *key, char *dst, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || !it->valuestring) return false;
    if (strncmp(dst, it->valuestring, cap) == 0) return false;
    strncpy(dst, it->valuestring, cap - 1);
    dst[cap - 1] = '\0';
    return true;
}

static void update_signature_flag(bbl_status_t *s)
{
    if (s->fun[0]) {
        unsigned long long v = strtoull(s->fun, NULL, 16);
        s->signature_required = (v & BBL_FUN_MQTT_SIGNATURE_REQUIRED) != 0;
    } else {
        s->signature_required = s->flag3_auth;
    }
}

// 解析命令回执：{"print":{"command":"pause","result":"failed","reason":"..."}}
static bool parse_command_result(bbl_status_t *s, const cJSON *sect)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(sect, "command");
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(sect, "result");
    if (!cJSON_IsString(cmd) || !cJSON_IsString(res)) return false;
    if (!strcmp(cmd->valuestring, "push_status")) return false;

    strncpy(s->result_cmd, cmd->valuestring, sizeof(s->result_cmd) - 1);
    s->result_cmd[sizeof(s->result_cmd) - 1] = '\0';
    s->result_ok = strcasecmp(res->valuestring, "success") == 0;
    s->result_reason[0] = '\0';
    upd_str(sect, "reason", s->result_reason, sizeof(s->result_reason));
    double ec = 0;
    s->result_err_code = get_number(sect, "err_code", &ec) ? (uint32_t)ec : 0;
    s->result_seq++;
    return true;
}

// 把一个料盘条目里出现的字段合并进 t（只发变化字段时其余保持不变）
static void merge_tray(bbl_tray_t *t, const cJSON *j)
{
    double v;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "tray_type");
    if (cJSON_IsString(type)) {
        t->loaded = type->valuestring[0] != 0;
        memset(t->type, 0, sizeof(t->type));
        strncpy(t->type, type->valuestring, sizeof(t->type) - 1);
    }
    const cJSON *col = cJSON_GetObjectItemCaseSensitive(j, "tray_color");
    if (cJSON_IsString(col) && strlen(col->valuestring) >= 6) {
        t->color = (uint32_t)(strtoul(col->valuestring, NULL, 16) >> (strlen(col->valuestring) >= 8 ? 8 : 0));
    }
    if (get_number(j, "remain", &v)) t->remain = (int)v;
}

static bool parse_vt_tray(bbl_status_t *s, const cJSON *j)
{
    bbl_tray_t nt = s->vt;
    merge_tray(&nt, j);
    nt.present = nt.loaded;
    if (memcmp(&nt, &s->vt, sizeof(nt)) == 0) return false;
    s->vt = nt;
    return true;
}

static bool parse_ams(bbl_status_t *s, const cJSON *ams)
{
    bool ch = false;
    double v;
    int now = s->tray_now;
    if (get_number(ams, "tray_now", &v)) now = (int)v;
    if (now != s->tray_now) {
        s->tray_now = now;
        ch = true;
    }
    const cJSON *units = cJSON_GetObjectItemCaseSensitive(ams, "ams");
    if (!cJSON_IsArray(units)) return ch;
    const cJSON *unit = cJSON_GetArrayItem(units, 0);
    if (!unit) {
        ch |= s->ams_trays != 0;
        s->ams_trays = 0;
        return ch;
    }
    const cJSON *trays = cJSON_GetObjectItemCaseSensitive(unit, "tray");
    if (!cJSON_IsArray(trays)) return ch;
    // 增量合并：A1 只发变化的字段；按 "id" 定位料位。只有 "id" 一个键的条目表示该料位为空
    int n = s->ams_trays;
    const cJSON *t;
    int pos = 0;
    cJSON_ArrayForEach(t, trays) {
        int id = pos++;
        if (get_number(t, "id", &v)) id = (int)v;
        if (id < 0 || id >= 4) continue;
        bbl_tray_t nt = s->tray[id];
        if (id >= s->ams_trays) {
            memset(&nt, 0, sizeof(nt));
            nt.remain = -1;
        }
        if (cJSON_GetArraySize(t) == 1) {
            memset(&nt, 0, sizeof(nt));
            nt.remain = -1;
        }
        if (cJSON_GetArraySize(t) > 1) nt.present = true;
        merge_tray(&nt, t);
        if (memcmp(&nt, &s->tray[id], sizeof(nt)) != 0) {
            s->tray[id] = nt;
            ch = true;
        }
        if (id + 1 > n) n = id + 1;
    }
    if (n != s->ams_trays) {
        s->ams_trays = n;
        ch = true;
    }
    // tray_exist_bits（十六进制，bit i = 第 i 个料位有料盘）比条目本身更可靠
    const cJSON *bits = cJSON_GetObjectItemCaseSensitive(ams, "tray_exist_bits");
    if (cJSON_IsString(bits)) {
        unsigned long b = strtoul(bits->valuestring, NULL, 16);
        for (int i = 0; i < s->ams_trays; i++) {
            bool p = (b >> i) & 1;
            if (p != s->tray[i].present) {
                s->tray[i].present = p;
                ch = true;
            }
        }
    }
    return ch;
}

static bool parse_print(bbl_status_t *s, const cJSON *p)
{
    bool ch = false;
    const cJSON *gs = cJSON_GetObjectItemCaseSensitive(p, "gcode_state");
    if (cJSON_IsString(gs)) {
        bbl_gcode_state_t st = parse_gcode_state(gs->valuestring);
        if (st != s->gcode_state) {
            s->gcode_state = st;
            ch = true;
        }
    }
    ch |= upd_int(p, "mc_percent", &s->percent);
    ch |= upd_int(p, "mc_remaining_time", &s->remaining_min);
    ch |= upd_int(p, "layer_num", &s->layer);
    ch |= upd_int(p, "total_layer_num", &s->total_layers);
    ch |= upd_int(p, "mc_print_stage", &s->stage);
    ch |= upd_float(p, "nozzle_temper", &s->nozzle);
    ch |= upd_float(p, "nozzle_target_temper", &s->nozzle_target);
    ch |= upd_float(p, "bed_temper", &s->bed);
    ch |= upd_float(p, "bed_target_temper", &s->bed_target);
    ch |= upd_int(p, "spd_lvl", &s->speed_level);
    ch |= upd_int(p, "spd_mag", &s->speed_mag);
    ch |= upd_str(p, "subtask_name", s->subtask, sizeof(s->subtask));
    ch |= upd_str(p, "gcode_file", s->gcode_file, sizeof(s->gcode_file));

    double v;
    if (get_number(p, "cooling_fan_speed", &v)) {
        int pct = (int)lround(v * 100.0 / 15.0);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (pct != s->part_fan_pct) {
            s->part_fan_pct = pct;
            ch = true;
        }
    }
    if (get_number(p, "print_error", &v) && (uint32_t)v != s->print_error) {
        s->print_error = (uint32_t)v;
        ch = true;
    }

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(p, "wifi_signal");
    if (cJSON_IsString(wifi) && wifi->valuestring) {
        int dbm = atoi(wifi->valuestring);
        if (dbm != s->wifi_dbm) {
            s->wifi_dbm = dbm;
            ch = true;
        }
    }

    const cJSON *hms = cJSON_GetObjectItemCaseSensitive(p, "hms");
    if (cJSON_IsArray(hms)) {
        int n = cJSON_GetArraySize(hms);
        if (n != s->hms_count) {
            s->hms_count = n;
            ch = true;
        }
    }

    const cJSON *lights = cJSON_GetObjectItemCaseSensitive(p, "lights_report");
    if (cJSON_IsArray(lights)) {
        const cJSON *l;
        cJSON_ArrayForEach(l, lights) {
            const cJSON *node = cJSON_GetObjectItemCaseSensitive(l, "node");
            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(l, "mode");
            if (cJSON_IsString(node) && cJSON_IsString(mode) && !strcmp(node->valuestring, "chamber_light")) {
                bool on = strcmp(mode->valuestring, "off") != 0;
                if (!s->has_light || on != s->light_on) ch = true;
                s->has_light = true;
                s->light_on = on;
            }
        }
    }

    if (get_number(p, "gcode_start_time", &v) && (int64_t)v != s->start_time) {
        s->start_time = (int64_t)v;
        ch = true;
    }

    const cJSON *ams = cJSON_GetObjectItemCaseSensitive(p, "ams");
    if (cJSON_IsObject(ams)) ch |= parse_ams(s, ams);
    const cJSON *vt = cJSON_GetObjectItemCaseSensitive(p, "vt_tray");
    if (cJSON_IsObject(vt)) ch |= parse_vt_tray(s, vt);

    bool sig_before = s->signature_required;
    upd_str(p, "fun", s->fun, sizeof(s->fun));
    if (get_number(p, "flag3", &v)) {
        s->flag3_auth = (((uint32_t)v) & BBL_FLAG3_AUTH_CONTROL) != 0;
    }
    update_signature_flag(s);
    ch |= sig_before != s->signature_required;

    return ch;
}

bool bbl_parse_report(bbl_status_t *s, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool ch = false;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "print");
    if (cJSON_IsObject(p)) {
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(p, "command");
        bool is_status = cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "push_status");
        if (is_status || !cJSON_IsString(cmd)) {
            ch |= parse_print(s, p);
            s->valid = true;
        } else {
            ch |= parse_command_result(s, p);
        }
    }
    const char *sections[] = {"system", "pushing", "info"};
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        const cJSON *sec = cJSON_GetObjectItemCaseSensitive(root, sections[i]);
        if (cJSON_IsObject(sec)) ch |= parse_command_result(s, sec);
    }

    cJSON_Delete(root);
    return ch;
}
