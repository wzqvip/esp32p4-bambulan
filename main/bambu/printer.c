#include "printer.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "bambu_cmd.h"

static const char *TAG = "printer";

#define RX_BUF_SIZE (96 * 1024)      // 带 AMS 的全量帧可能超过 20KB，留足余量
#define RESYNC_PERIOD_US (5LL * 60 * 1000 * 1000)
#define STALE_US (90LL * 1000 * 1000)

extern const char bambu_ca_pem_start[] asm("_binary_bambu_ca_pem_start");

typedef struct {
    printer_cfg_t cfg;
    esp_mqtt_client_handle_t client;
    SemaphoreHandle_t lock;
    printer_snapshot_t snap;
    char topic_report[48];
    char topic_request[48];
    char client_id[40];
    char uri[64];
    char *rx_buf;
    size_t rx_len;
    bool rx_overflow;
    uint32_t seq;
    int64_t last_pushall_us;
    bool started;
} printer_t;

static printer_t s_printers[PRINTER_MAX];
static int s_count;
static esp_timer_handle_t s_resync_timer;

const char *printer_link_name(printer_link_t l)
{
    switch (l) {
    case PRINTER_LINK_NOT_CONFIGURED: return "未配置";
    case PRINTER_LINK_WAIT_NETWORK: return "等待网络";
    case PRINTER_LINK_CONNECTING: return "连接中";
    case PRINTER_LINK_ONLINE: return "在线";
    case PRINTER_LINK_AUTH_FAILED: return "访问码错误";
    case PRINTER_LINK_TLS_FAILED: return "TLS 失败";
    case PRINTER_LINK_OFFLINE: return "离线";
    default: return "?";
    }
}

static void set_link(printer_t *p, printer_link_t l)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    p->snap.link = l;
    xSemaphoreGive(p->lock);
}

static esp_err_t publish(printer_t *p, const char *payload, int len)
{
    if (!p->client || len <= 0) return ESP_ERR_INVALID_ARG;
    if (p->snap.link != PRINTER_LINK_ONLINE) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "[%s] -> %.*s", p->cfg.name, len, payload);
    int id = esp_mqtt_client_enqueue(p->client, p->topic_request, payload, len, 0, 0, true);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

static void send_pushall(printer_t *p)
{
    char buf[128];
    int n = bbl_cmd_pushall(buf, sizeof(buf), ++p->seq);
    p->last_pushall_us = esp_timer_get_time();
    publish(p, buf, n);
}

static void handle_message(printer_t *p)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    bbl_parse_report(&p->snap.st, p->rx_buf, p->rx_len);
    p->snap.rx_count++;
    p->snap.last_rx_us = esp_timer_get_time();
    xSemaphoreGive(p->lock);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    printer_t *p = arg;
    esp_mqtt_event_handle_t ev = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        if (p->snap.link != PRINTER_LINK_AUTH_FAILED && p->snap.link != PRINTER_LINK_TLS_FAILED) {
            set_link(p, PRINTER_LINK_CONNECTING);
        }
        break;

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[%s] MQTT connected", p->cfg.name);
        set_link(p, PRINTER_LINK_ONLINE);
        esp_mqtt_client_subscribe(p->client, p->topic_report, 0);
        send_pushall(p);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[%s] MQTT disconnected", p->cfg.name);
        if (p->snap.link == PRINTER_LINK_ONLINE || p->snap.link == PRINTER_LINK_CONNECTING) {
            set_link(p, PRINTER_LINK_OFFLINE);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (!ev->error_handle) break;
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "[%s] connection refused, code=%d", p->cfg.name, ev->error_handle->connect_return_code);
            set_link(p, PRINTER_LINK_AUTH_FAILED);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            int tls_err = ev->error_handle->esp_tls_stack_err;
            esp_err_t last = ev->error_handle->esp_tls_last_esp_err;
            ESP_LOGE(TAG, "[%s] transport error: esp_err=0x%x tls=-0x%x sock_errno=%d", p->cfg.name, last, -tls_err,
                     ev->error_handle->esp_transport_sock_errno);
            bool tls_problem = last == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED ||
                               last == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED ||
                               ev->error_handle->esp_tls_cert_verify_flags != 0;
            set_link(p, tls_problem ? PRINTER_LINK_TLS_FAILED : PRINTER_LINK_OFFLINE);
        }
        break;

    case MQTT_EVENT_DATA:
        // 大报文会被拆成多段送来，这里按 offset 拼回去
        if (ev->current_data_offset == 0) {
            p->rx_len = 0;
            p->rx_overflow = ev->total_data_len >= RX_BUF_SIZE;
        }
        if (!p->rx_overflow && ev->current_data_offset + ev->data_len <= RX_BUF_SIZE - 1) {
            memcpy(p->rx_buf + ev->current_data_offset, ev->data, ev->data_len);
            p->rx_len = ev->current_data_offset + ev->data_len;
        }
        if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
            if (p->rx_overflow) {
                ESP_LOGW(TAG, "[%s] message too large (%d), dropped", p->cfg.name, ev->total_data_len);
            } else {
                p->rx_buf[p->rx_len] = '\0';
                handle_message(p);
            }
        }
        break;

    default:
        break;
    }
}

static void resync_timer_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_count; i++) {
        printer_t *p = &s_printers[i];
        if (p->snap.link != PRINTER_LINK_ONLINE) continue;
        bool stale = now - p->snap.last_rx_us > STALE_US;
        if (now - p->last_pushall_us > RESYNC_PERIOD_US || (stale && now - p->last_pushall_us > STALE_US)) {
            send_pushall(p);
        }
    }
}

static void load_cfg(int idx, printer_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    if (idx == 0) {
        strlcpy(c->name, CONFIG_BBL_P1_NAME, sizeof(c->name));
        strlcpy(c->host, CONFIG_BBL_P1_HOST, sizeof(c->host));
        strlcpy(c->serial, CONFIG_BBL_P1_SERIAL, sizeof(c->serial));
        strlcpy(c->access_code, CONFIG_BBL_P1_ACCESS_CODE, sizeof(c->access_code));
    }
#if CONFIG_BBL_PRINTER_COUNT >= 2
    if (idx == 1) {
        strlcpy(c->name, CONFIG_BBL_P2_NAME, sizeof(c->name));
        strlcpy(c->host, CONFIG_BBL_P2_HOST, sizeof(c->host));
        strlcpy(c->serial, CONFIG_BBL_P2_SERIAL, sizeof(c->serial));
        strlcpy(c->access_code, CONFIG_BBL_P2_ACCESS_CODE, sizeof(c->access_code));
    }
#endif
}

esp_err_t printers_init(void)
{
    s_count = CONFIG_BBL_PRINTER_COUNT;
    if (s_count > PRINTER_MAX) s_count = PRINTER_MAX;

    for (int i = 0; i < s_count; i++) {
        printer_t *p = &s_printers[i];
        memset(p, 0, sizeof(*p));
        load_cfg(i, &p->cfg);
        p->lock = xSemaphoreCreateMutex();
        bbl_status_init(&p->snap.st);
        if (!p->cfg.host[0] || !p->cfg.serial[0] || !p->cfg.access_code[0]) {
            ESP_LOGW(TAG, "printer %d not configured (idf.py menuconfig -> 拓竹屏配置)", i + 1);
            p->snap.link = PRINTER_LINK_NOT_CONFIGURED;
            continue;
        }
        p->snap.link = PRINTER_LINK_WAIT_NETWORK;
        p->rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p->rx_buf) return ESP_ERR_NO_MEM;

        snprintf(p->topic_report, sizeof(p->topic_report), "device/%s/report", p->cfg.serial);
        snprintf(p->topic_request, sizeof(p->topic_request), "device/%s/request", p->cfg.serial);
        snprintf(p->client_id, sizeof(p->client_id), "p4screen-%08lx-%d", (unsigned long)esp_random(), i);
        snprintf(p->uri, sizeof(p->uri), "mqtts://%s:8883", p->cfg.host);

        esp_mqtt_client_config_t mc = {
            .broker.address.uri = p->uri,
            .credentials.username = "bblp",
            .credentials.client_id = p->client_id,
            .credentials.authentication.password = p->cfg.access_code,
            .session.keepalive = 30,
            .session.disable_clean_session = false,
            .network.reconnect_timeout_ms = 5000,
            .network.timeout_ms = 15000,
            .buffer.size = 8192,
            .buffer.out_size = 1024,
            .task.stack_size = 8192,
        };
#if CONFIG_BBL_TLS_VERIFY
        mc.broker.verification.certificate = bambu_ca_pem_start;
        mc.broker.verification.common_name = p->cfg.serial;   // 打印机证书的 CN 是序列号
#else
        mc.broker.verification.skip_cert_common_name_check = true;
#endif
        p->client = esp_mqtt_client_init(&mc);
        if (!p->client) return ESP_FAIL;
        esp_mqtt_client_register_event(p->client, ESP_EVENT_ANY_ID, mqtt_event_handler, p);
    }

    const esp_timer_create_args_t targs = {.callback = resync_timer_cb, .name = "bbl_resync"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_resync_timer));
    return ESP_OK;
}

void printers_start(void)
{
    for (int i = 0; i < s_count; i++) {
        printer_t *p = &s_printers[i];
        if (!p->client || p->started) continue;
        ESP_LOGI(TAG, "[%s] connecting to %s (SN %s)", p->cfg.name, p->uri, p->cfg.serial);
        set_link(p, PRINTER_LINK_CONNECTING);
        if (esp_mqtt_client_start(p->client) == ESP_OK) p->started = true;
    }
    if (!esp_timer_is_active(s_resync_timer)) esp_timer_start_periodic(s_resync_timer, 30LL * 1000 * 1000);
}

int printers_count(void) { return s_count; }

const printer_cfg_t *printer_cfg(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_printers[idx].cfg : NULL;
}

bool printer_get(int idx, printer_snapshot_t *out)
{
    if (idx < 0 || idx >= s_count) return false;
    printer_t *p = &s_printers[idx];
    xSemaphoreTake(p->lock, portMAX_DELAY);
    *out = p->snap;
    xSemaphoreGive(p->lock);
    return true;
}

#define CMD_BUF 256
#define DEFINE_CMD(fn, builder_call)                         \
    esp_err_t fn                                             \
    {                                                        \
        if (idx < 0 || idx >= s_count) return ESP_ERR_INVALID_ARG; \
        printer_t *p = &s_printers[idx];                     \
        char buf[CMD_BUF];                                   \
        uint32_t seq = ++p->seq;                             \
        int n = builder_call;                                \
        return publish(p, buf, n);                           \
    }

DEFINE_CMD(printer_pause(int idx), bbl_cmd_pause(buf, sizeof(buf), seq))
DEFINE_CMD(printer_resume(int idx), bbl_cmd_resume(buf, sizeof(buf), seq))
DEFINE_CMD(printer_stop(int idx), bbl_cmd_stop(buf, sizeof(buf), seq))
DEFINE_CMD(printer_set_speed(int idx, int level), bbl_cmd_speed(buf, sizeof(buf), seq, level))
DEFINE_CMD(printer_set_light(int idx, bool on), bbl_cmd_light(buf, sizeof(buf), seq, on))
DEFINE_CMD(printer_set_nozzle_temp(int idx, int c), bbl_cmd_nozzle_temp(buf, sizeof(buf), seq, c))
DEFINE_CMD(printer_set_bed_temp(int idx, int c), bbl_cmd_bed_temp(buf, sizeof(buf), seq, c))
DEFINE_CMD(printer_set_part_fan(int idx, int pct), bbl_cmd_part_fan(buf, sizeof(buf), seq, pct))

esp_err_t printer_request_full_status(int idx)
{
    if (idx < 0 || idx >= s_count) return ESP_ERR_INVALID_ARG;
    send_pushall(&s_printers[idx]);
    return ESP_OK;
}
