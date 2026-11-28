// 演示机器：不连网络的虚拟打印机，状态在本地按时间推进，用来展示界面或测试布局。
// 在设置页「打印机」里选数量（0..DEMO_MAX），排在真实打印机后面。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "printer.h"

#define DEMO_MAX 4

typedef enum {
    DEMO_CMD_PAUSE,
    DEMO_CMD_RESUME,
    DEMO_CMD_STOP,
    DEMO_CMD_SPEED,
    DEMO_CMD_LIGHT,
    DEMO_CMD_NOZZLE,
    DEMO_CMD_BED,
    DEMO_CMD_FAN,
} demo_cmd_t;

/** 第 d 台演示机器当前的状态（now_ms 是单调时间，unix_now 是当前 Unix 秒，0 表示不知道） */
void demo_get(int d, int64_t now_ms, int64_t unix_now, printer_snapshot_t *out);
const char *demo_name(int d);
void demo_cmd(int d, demo_cmd_t cmd, int val);
