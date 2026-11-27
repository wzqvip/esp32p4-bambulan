// 每台打印机一个 MQTT(TLS) 连接，状态在后台线程更新，UI 线程按需拷贝读取
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "bambu_parse.h"

#define PRINTER_MAX 2

typedef enum {
    PRINTER_LINK_NOT_CONFIGURED = 0,
    PRINTER_LINK_WAIT_NETWORK,
    PRINTER_LINK_CONNECTING,
    PRINTER_LINK_ONLINE,
    PRINTER_LINK_AUTH_FAILED,   // 访问码错误
    PRINTER_LINK_TLS_FAILED,    // TLS 握手/证书失败
    PRINTER_LINK_OFFLINE,       // 连不上或断开，正在重连
} printer_link_t;

typedef struct {
    char name[32];
    char host[40];
    char serial[24];
    char access_code[16];
} printer_cfg_t;

typedef struct {
    printer_link_t link;
    bbl_status_t st;
    uint32_t rx_count;       // 收到的报文数
    int64_t last_rx_us;      // 最近一次收到报文的时间
} printer_snapshot_t;

/** 从 Kconfig 读配置并创建客户端（不连接） */
esp_err_t printers_init(void);
int printers_count(void);
const printer_cfg_t *printer_cfg(int idx);

/** 网络就绪后调用，开始连接所有打印机 */
void printers_start(void);

/** 拷贝一份当前状态，UI 线程安全 */
bool printer_get(int idx, printer_snapshot_t *out);

const char *printer_link_name(printer_link_t l);

// 控制命令（非阻塞，入队发送）
esp_err_t printer_pause(int idx);
esp_err_t printer_resume(int idx);
esp_err_t printer_stop(int idx);
esp_err_t printer_set_speed(int idx, int level);
esp_err_t printer_set_light(int idx, bool on);
esp_err_t printer_set_nozzle_temp(int idx, int celsius);
esp_err_t printer_set_bed_temp(int idx, int celsius);
esp_err_t printer_set_part_fan(int idx, int percent);
esp_err_t printer_request_full_status(int idx);
