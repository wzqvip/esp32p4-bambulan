// 用户设置（NVS 命名空间 "bbl_set"，键 "cfg"，整份结构体存成一个 blob）
#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "board.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
static const char *TAG = "settings";
#endif

// 模拟器里没有 sdkconfig：给出同样的默认值
#ifndef CONFIG_BBL_BACKLIGHT_PERCENT
#define CONFIG_BBL_BACKLIGHT_PERCENT 100
#define CONFIG_BBL_TIMEZONE "CST-8"
#define CONFIG_BBL_PAGE_ANIM_MS 0
#define CONFIG_BBL_WIFI_SSID "MyWiFi"
#define CONFIG_BBL_WIFI_PASSWORD ""
#define CONFIG_BBL_PRINTER_COUNT 2
#define CONFIG_BBL_P1_NAME "A1 #1"
#define CONFIG_BBL_P1_HOST "192.168.1.31"
#define CONFIG_BBL_P1_SERIAL "03919A3B0100001"
#define CONFIG_BBL_P1_ACCESS_CODE "12345678"
#define CONFIG_BBL_P2_NAME "A1 #2"
#define CONFIG_BBL_P2_HOST "192.168.1.32"
#define CONFIG_BBL_P2_SERIAL "03919A3B0100002"
#define CONFIG_BBL_P2_ACCESS_CODE "87654321"
#endif

#define SETTINGS_MAGIC 0x42424c01u   // 结构体改了就换版本号，旧数据作废

typedef struct {
    uint32_t magic;
    uint32_t size;
    settings_t s;
} blob_t;

static settings_t s_cur;

const settings_tz_t settings_tz_list[] = {
    {"北京 / 上海 / 香港 / 台北 (UTC+8)", "CST-8"},
    {"东京 / 首尔 (UTC+9)", "JST-9"},
    {"悉尼 (UTC+10，夏令时)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"曼谷 / 雅加达 (UTC+7)", "<+07>-7"},
    {"新德里 (UTC+5:30)", "IST-5:30"},
    {"迪拜 (UTC+4)", "<+04>-4"},
    {"莫斯科 (UTC+3)", "MSK-3"},
    {"柏林 / 巴黎 (UTC+1，夏令时)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"伦敦 (UTC+0，夏令时)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"UTC", "UTC0"},
    {"纽约 (UTC-5，夏令时)", "EST5EDT,M3.2.0,M11.1.0"},
    {"芝加哥 (UTC-6，夏令时)", "CST6CDT,M3.2.0,M11.1.0"},
    {"丹佛 (UTC-7，夏令时)", "MST7MDT,M3.2.0,M11.1.0"},
    {"洛杉矶 (UTC-8，夏令时)", "PST8PDT,M3.2.0,M11.1.0"},
    {NULL, NULL},
};

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = CONFIG_BBL_BACKLIGHT_PERCENT;
    s->sleep_min = 0;
    s->sleep_level = 0;
    s->sleep_when_printing = false;
    s->page_anim = CONFIG_BBL_PAGE_ANIM_MS > 0;
    s->camera_on = true;
    s->perf_overlay = true;
    snprintf(s->tz, sizeof(s->tz), "%s", CONFIG_BBL_TIMEZONE);
    snprintf(s->wifi_ssid, sizeof(s->wifi_ssid), "%s", CONFIG_BBL_WIFI_SSID);
    snprintf(s->wifi_pass, sizeof(s->wifi_pass), "%s", CONFIG_BBL_WIFI_PASSWORD);
    settings_printer_t *p = &s->printer[0];
    snprintf(p->name, sizeof(p->name), "%s", CONFIG_BBL_P1_NAME);
    snprintf(p->host, sizeof(p->host), "%s", CONFIG_BBL_P1_HOST);
    snprintf(p->serial, sizeof(p->serial), "%s", CONFIG_BBL_P1_SERIAL);
    snprintf(p->access_code, sizeof(p->access_code), "%s", CONFIG_BBL_P1_ACCESS_CODE);
#if CONFIG_BBL_PRINTER_COUNT >= 2
    p = &s->printer[1];
    snprintf(p->name, sizeof(p->name), "%s", CONFIG_BBL_P2_NAME);
    snprintf(p->host, sizeof(p->host), "%s", CONFIG_BBL_P2_HOST);
    snprintf(p->serial, sizeof(p->serial), "%s", CONFIG_BBL_P2_SERIAL);
    snprintf(p->access_code, sizeof(p->access_code), "%s", CONFIG_BBL_P2_ACCESS_CODE);
#endif
}

void settings_init(void)
{
    defaults(&s_cur);
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("bbl_set", NVS_READONLY, &h) == ESP_OK) {
        blob_t *b = calloc(1, sizeof(*b));
        size_t len = sizeof(*b);
        // 新版本只会往 settings_t 末尾加字段：旧数据比现在短时，读出前面那部分，其余保持默认
        size_t hdr = offsetof(blob_t, s);
        if (b && nvs_get_blob(h, "cfg", b, &len) == ESP_OK && len > hdr && b->magic == SETTINGS_MAGIC &&
            b->size == len - hdr && b->size <= sizeof(settings_t)) {
            memcpy(&s_cur, &b->s, b->size);
            s_cur.tz[sizeof(s_cur.tz) - 1] = 0;
            ESP_LOGI(TAG, "loaded from NVS (%u bytes)", (unsigned)b->size);
        } else {
            ESP_LOGI(TAG, "no saved settings, using menuconfig defaults");
        }
        free(b);
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "no saved settings, using menuconfig defaults");
    }
    if (s_cur.brightness < 5 || s_cur.brightness > 100) s_cur.brightness = CONFIG_BBL_BACKLIGHT_PERCENT;
#endif
    settings_apply_tz();
}

const settings_t *settings_get(void) { return &s_cur; }

void settings_apply_tz(void)
{
    setenv("TZ", s_cur.tz[0] ? s_cur.tz : CONFIG_BBL_TIMEZONE, 1);
    tzset();
}

void settings_save(const settings_t *s)
{
    bool tz_changed = strcmp(s->tz, s_cur.tz) != 0;
    if (s != &s_cur) s_cur = *s;
    if (tz_changed) settings_apply_tz();
#ifdef ESP_PLATFORM
    blob_t *b = calloc(1, sizeof(*b));
    if (!b) return;
    b->magic = SETTINGS_MAGIC;
    b->size = sizeof(settings_t);
    b->s = s_cur;
    nvs_handle_t h;
    esp_err_t err = nvs_open("bbl_set", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "cfg", b, sizeof(*b));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(b);
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
#endif
}

void settings_reset(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("bbl_set", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
#endif
    defaults(&s_cur);
    settings_apply_tz();
}

#ifdef ESP_PLATFORM
void sys_set_backlight(int percent) { board_set_backlight(percent); }

void sys_restart(void) { esp_restart(); }

void sys_info(char *buf, size_t cap)
{
    const esp_app_desc_t *d = esp_app_get_description();
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(buf, cap,
             "固件版本：%s\n编译时间：%s %s\nESP-IDF：%s\n运行时间：%d天 %02d:%02d:%02d\n"
             "可用内存：内部 %u KB，PSRAM %u KB",
             d->version, d->date, d->time, d->idf_ver, (int)(up / 86400), (int)(up / 3600 % 24),
             (int)(up / 60 % 60), (int)(up % 60), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
#endif
