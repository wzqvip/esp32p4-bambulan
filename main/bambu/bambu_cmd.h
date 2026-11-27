// 拓竹局域网 MQTT 命令（发往 device/<SN>/request）。只拼字符串，不依赖 ESP-IDF。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 所有函数返回写入的长度（不含结尾 0），buf 不够时返回 -1
int bbl_cmd_pushall(char *buf, size_t cap, uint32_t seq);
int bbl_cmd_pause(char *buf, size_t cap, uint32_t seq);
int bbl_cmd_resume(char *buf, size_t cap, uint32_t seq);
int bbl_cmd_stop(char *buf, size_t cap, uint32_t seq);
int bbl_cmd_speed(char *buf, size_t cap, uint32_t seq, int level);        // 1 静音 2 标准 3 运动 4 狂暴
int bbl_cmd_light(char *buf, size_t cap, uint32_t seq, bool on);          // chamber_light（A1 是工具头灯）
int bbl_cmd_nozzle_temp(char *buf, size_t cap, uint32_t seq, int celsius);
int bbl_cmd_bed_temp(char *buf, size_t cap, uint32_t seq, int celsius);
int bbl_cmd_part_fan(char *buf, size_t cap, uint32_t seq, int percent);   // 模型冷却风扇 P1
int bbl_cmd_gcode(char *buf, size_t cap, uint32_t seq, const char *gcode); // gcode 不能含引号/反斜杠

#ifdef __cplusplus
}
#endif
