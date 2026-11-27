// 拓竹打印机局域网 MQTT 报文解析（纯 C + cJSON，可在电脑上单元测试）
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BBL_GS_UNKNOWN = 0,
    BBL_GS_IDLE,
    BBL_GS_PREPARE,
    BBL_GS_RUNNING,
    BBL_GS_PAUSE,
    BBL_GS_FINISH,
    BBL_GS_FAILED,
    BBL_GS_SLICING,
} bbl_gcode_state_t;

typedef enum {
    BBL_SPEED_SILENT = 1,
    BBL_SPEED_STANDARD = 2,
    BBL_SPEED_SPORT = 3,
    BBL_SPEED_LUDICROUS = 4,
} bbl_speed_t;

typedef struct {
    bool loaded;
    uint32_t color;   // RRGGBB
    char type[12];    // PLA / PETG ...
    int remain;       // 余量 %，-1 未知
} bbl_tray_t;

typedef struct {
    bool valid;           // 收到过至少一条状态
    bbl_gcode_state_t gcode_state;
    int percent;          // mc_percent 0..100
    int remaining_min;    // mc_remaining_time，分钟
    int layer;            // layer_num
    int total_layers;     // total_layer_num
    int stage;            // mc_print_stage
    float nozzle, nozzle_target;
    float bed, bed_target;
    int part_fan_pct;     // cooling_fan_speed (0..15) 换算成百分比
    int speed_level;      // spd_lvl 1..4
    int speed_mag;        // spd_mag，百分比
    bool has_light;
    bool light_on;        // lights_report: chamber_light
    char subtask[96];     // subtask_name（任务名）
    char gcode_file[96];
    uint32_t print_error;
    int hms_count;
    int wifi_dbm;
    int64_t start_time;   // gcode_start_time（Unix 秒），0 表示未知

    // AMS（A1 是 AMS lite，4 个料位）。只取第一台 AMS。
    int ams_trays;        // 0 表示没有 AMS
    bbl_tray_t tray[4];
    int tray_now;         // 当前料位 0..3；254 外挂料盘；255 无

    // 授权控制：为 true 表示打印机要求签名命令（未开开发者模式），第三方只能读
    bool signature_required;
    bool flag3_auth;      // 来自 flag3 bit 4（只在全量帧出现，需跨帧保留）
    char fun[20];         // 来自 print.fun（十六进制字符串）

    // 最近一次命令回执
    uint32_t result_seq;  // 每收到一次回执 +1
    char result_cmd[24];
    bool result_ok;
    char result_reason[64];
    uint32_t result_err_code;
} bbl_status_t;

void bbl_status_init(bbl_status_t *s);

/**
 * 把一条 device/<SN>/report 报文合并进 s（A1/P1 系列只发变化的字段，所以是增量合并）。
 * @return true 表示 s 有字段被更新
 */
bool bbl_parse_report(bbl_status_t *s, const char *json, size_t len);

const char *bbl_gcode_state_name(bbl_gcode_state_t st);   // 中文状态名
bool bbl_state_is_active(bbl_gcode_state_t st);           // 打印中/暂停/准备中

#ifdef __cplusplus
}
#endif
