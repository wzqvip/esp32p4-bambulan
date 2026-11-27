#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    NET_NO_CONFIG = 0,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_DISCONNECTED,
} net_state_t;

typedef void (*net_ready_cb_t)(void);

/** 启动 Wi-Fi（经板载 ESP32-C6，esp_hosted/esp_wifi_remote）与 SNTP。拿到 IP 时调用 cb。 */
esp_err_t net_start(net_ready_cb_t cb);
net_state_t net_state(void);
int net_rssi(void);              // 未连接时返回 0
bool net_time_valid(void);       // SNTP 已同步
void net_ip_str(char *buf, int len);
