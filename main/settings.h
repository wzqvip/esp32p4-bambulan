// 用户设置：开机从 NVS 读取，没存过的项用 menuconfig 里的值作默认。
// 屏幕上的设置页修改后调用 settings_save() 写回 NVS。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_PRINTERS 2

typedef struct {
    char name[32];
    char host[40];
    char serial[24];
    char access_code[16];
} settings_printer_t;

typedef struct {
    // 显示
    uint8_t brightness;      // 背光 5..100 %
    uint16_t sleep_min;      // 无操作多少分钟后息屏，0 = 从不
    uint8_t sleep_level;     // 息屏时的亮度：0 = 关背光，否则百分比
    bool sleep_when_printing;// 打印中也息屏（关掉则打印时常亮）
    bool page_anim;          // 切页滑动动画
    bool camera_on;          // 实时画面
    // 时间
    char tz[48];             // POSIX TZ
    // 网络
    char wifi_ssid[33];
    char wifi_pass[65];
    // 打印机
    settings_printer_t printer[SETTINGS_PRINTERS];
    // ---- 以下是后加的字段：只能往结构体末尾加，旧版本存的数据会保留前面部分 ----
    bool perf_overlay;       // 右下角显示帧率 / CPU
} settings_t;

/** 从 NVS 读取设置（在 nvs_flash_init 之后、其他模块之前调用） */
void settings_init(void);

/** 当前设置（只读） */
const settings_t *settings_get(void);

/** 保存整份设置到 NVS，并立即应用亮度和时区 */
void settings_save(const settings_t *s);

/** 清除 NVS 里的设置（重启后恢复 menuconfig 默认值） */
void settings_reset(void);

/** 应用时区（setenv TZ + tzset） */
void settings_apply_tz(void);

typedef struct {
    const char *label;
    const char *tz;
} settings_tz_t;

/** 时区选项表，以 {NULL, NULL} 结尾 */
extern const settings_tz_t settings_tz_list[];

// ---- 平台相关（板子上在 settings.c 实现，模拟器里打桩） ----
void sys_set_backlight(int percent);
void sys_restart(void);
/** 固件版本、运行时间、内存等，多行文本 */
void sys_info(char *buf, size_t cap);
