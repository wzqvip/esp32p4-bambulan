// 在电脑上渲染界面截图（不需要板子），输出 build/*.ppm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "camera.h"
#include "cover.h"
#include "lvgl.h"
#include "net.h"
#include "printer.h"
#include "ui.h"
#include "settings.h"

static int64_t s_ms;
int64_t esp_timer_get_time(void) { return s_ms * 1000; }

// ---- 假打印机 ----
static printer_cfg_t cfgs[2] = {{"A1 一号", "192.168.1.31", "03919A3B0100001", "12345678"},
                                {"A1 二号", "192.168.1.32", "03919A3B0100002", "87654321"}};
static printer_snapshot_t snaps[2];
int printers_count(void) { return 2; }
const printer_cfg_t *printer_cfg(int i) { return (i >= 0 && i < 2) ? &cfgs[i] : NULL; }
bool printer_get(int i, printer_snapshot_t *o)
{
    *o = snaps[i];
    return true;
}
const char *printer_link_name(printer_link_t l)
{
    static const char *n[] = {"未配置", "等待网络", "连接中", "在线", "访问码错误", "TLS 失败", "离线", "连接已满"};
    return n[l];
}
const char *printer_display_name(int i, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", i == 1 ? "塔可的A1" : cfgs[i].name);
    return buf;
}
static const char *s_last_cmd = "";
#define STUB(name, sig)  \
    esp_err_t sig        \
    {                    \
        s_last_cmd = name; \
        printf("cmd: %s\n", name); \
        return ESP_OK;   \
    }
STUB("pause", printer_pause(int i))
STUB("resume", printer_resume(int i))
STUB("stop", printer_stop(int i))
STUB("speed", printer_set_speed(int i, int l))
STUB("light", printer_set_light(int i, bool on))
STUB("nozzle", printer_set_nozzle_temp(int i, int c))
STUB("bed", printer_set_bed_temp(int i, int c))
STUB("fan", printer_set_part_fan(int i, int p))
STUB("pushall", printer_request_full_status(int i))
net_state_t net_state(void) { return NET_CONNECTED; }
int net_rssi(void) { return -52; }
bool net_time_valid(void) { return true; }
void net_ip_str(char *b, int n) { snprintf(b, n, "192.168.1.50"); }
void sys_set_backlight(int p) { printf("backlight %d\n", p); }
void sys_restart(void) { printf("restart\n"); }
void sys_info(char *b, size_t n)
{
    snprintf(b, n, "固件版本：sim\n编译时间：-\nESP-IDF：-\n运行时间：0天 00:01:23\n可用内存：内部 312 KB，PSRAM 28123 KB");
}


// ---- 假摄像头：一号机给一张渐变图当画面，二号机模拟被拒 ----
static uint16_t s_frame[CAMERA_VIEW_W * CAMERA_VIEW_H];
static lv_image_dsc_t s_frame_dsc;
static lv_obj_t *s_cam_imgs[2][CAMERA_MAX_BIND];
static int s_cam_n[2];
void camera_bind_image(int idx, lv_obj_t *img)
{
    if (idx >= 0 && idx < 2 && s_cam_n[idx] < CAMERA_MAX_BIND) s_cam_imgs[idx][s_cam_n[idx]++] = img;
}
void camera_start_all(void) {}
camera_state_t camera_state(int idx) { return idx == 0 ? CAMERA_STREAMING : CAMERA_REJECTED; }
const char *camera_state_text(camera_state_t st)
{
    return st == CAMERA_REJECTED ? "打印机拒绝了视频连接\n（被 Bambu Studio 占用，或访问码错误）" : "";
}
bool camera_has_frame(int idx) { return idx == 0; }
static void fake_camera_frame(void)
{
    for (int y = 0; y < CAMERA_VIEW_H; y++)
        for (int x = 0; x < CAMERA_VIEW_W; x++) {
            int r = 40 + x * 60 / CAMERA_VIEW_W, g = 50 + y * 70 / CAMERA_VIEW_H, b = 70;
            int dx = x - 240, dy = y - 170;
            if (dx * dx / 4 + dy * dy < 900) r = 220, g = 120, b = 40;   // 一块“模型”
            if (y > 200) r = g = b = 30 + (x / 24 % 2) * 12;            // 热床纹理
            s_frame[y * CAMERA_VIEW_W + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    s_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_frame_dsc.header.w = CAMERA_VIEW_W;
    s_frame_dsc.header.h = CAMERA_VIEW_H;
    s_frame_dsc.header.stride = CAMERA_VIEW_W * 2;
    s_frame_dsc.data = (const uint8_t *)s_frame;
    s_frame_dsc.data_size = sizeof(s_frame);
    for (int i = 0; i < s_cam_n[0]; i++) lv_image_set_src(s_cam_imgs[0][i], &s_frame_dsc);
}


// ---- 假预览图：一号机给一个“模型”剪影 ----
static uint32_t s_cover[COVER_SIZE * COVER_SIZE];
static lv_image_dsc_t s_cover_dsc;
static lv_obj_t *s_cov_imgs[2][COVER_MAX_BIND];
static int s_cov_n[2];
void cover_bind_image(int idx, lv_obj_t *img)
{
    if (idx >= 0 && idx < 2 && s_cov_n[idx] < COVER_MAX_BIND) s_cov_imgs[idx][s_cov_n[idx]++] = img;
}
void cover_start_all(void) {}
bool cover_has_image(int idx) { return idx == 0; }
static void fake_cover(void)
{
    for (int y = 0; y < COVER_SIZE; y++)
        for (int x = 0; x < COVER_SIZE; x++) {
            int dx = x - 64, dy = y - 70;
            bool body = dx * dx + dy * dy * 2 < 1500, head = (x - 84) * (x - 84) + (y - 40) * (y - 40) < 200;
            s_cover[y * COVER_SIZE + x] = (body || head) ? 0xFF4CAF50u : 0;
        }
    s_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cover_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_cover_dsc.header.w = COVER_SIZE;
    s_cover_dsc.header.h = COVER_SIZE;
    s_cover_dsc.header.stride = COVER_SIZE * 4;
    s_cover_dsc.data = (const uint8_t *)s_cover;
    s_cover_dsc.data_size = sizeof(s_cover);
    for (int i = 0; i < s_cov_n[0]; i++) lv_image_set_src(s_cov_imgs[0][i], &s_cover_dsc);
}

// ---- 显示 + 触摸 ----
#define W 1024
#define H 600
static uint32_t fb[W * H];
static uint32_t drawbuf[W * H];

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) memcpy(&fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 4, (size_t)w * 4);
    lv_display_flush_ready(d);
}

static int tx, ty, tpressed;
static void touch_read(lv_indev_t *i, lv_indev_data_t *d)
{
    d->point.x = tx;
    d->point.y = ty;
    d->state = tpressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        s_ms += 5;
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void tap(int x, int y)
{
    tx = x, ty = y, tpressed = 1;
    run(100);
    tpressed = 0;
    run(100);
}

static void shot(const char *name)
{
    lv_refr_now(NULL);
    char path[128];
    snprintf(path, sizeof(path), "%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint8_t rgb[3] = {(fb[i] >> 16) & 0xFF, (fb[i] >> 8) & 0xFF, fb[i] & 0xFF};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

int main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_buffers(d, drawbuf, NULL, sizeof(drawbuf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);

    // 一号机：打印中
    bbl_status_t *a = &snaps[0].st;
    bbl_status_init(a);
    snaps[0].link = PRINTER_LINK_ONLINE;
    a->valid = true;
    a->gcode_state = BBL_GS_RUNNING;
    a->percent = 63;
    a->remaining_min = 87;
    a->layer = 142;
    a->total_layers = 230;
    a->nozzle = 219.6f, a->nozzle_target = 220;
    a->bed = 64.8f, a->bed_target = 65;
    a->part_fan_pct = 100;
    a->has_light = true, a->light_on = true;
    strcpy(a->subtask, "龙年摆件_0.2mm_PLA_2h30m");
    a->start_time = (int64_t)time(NULL) - 92 * 60;
    a->ams_trays = 4;
    a->tray_now = 1;
    const struct { const char *t; uint32_t c; int r; } trays[4] = {
        {"PLA", 0xF2F2F2, 80}, {"PLA", 0xD32F2F, 45}, {"PETG", 0x1E88E5, 100}, {"", 0, -1}};
    for (int i = 0; i < 4; i++) {
        a->tray[i].loaded = trays[i].t[0] != 0;
        strcpy(a->tray[i].type, trays[i].t);
        a->tray[i].color = trays[i].c;
        a->tray[i].remain = trays[i].r;
    }

    // 二号机：暂停，未开开发者模式
    bbl_status_t *b = &snaps[1].st;
    bbl_status_init(b);
    snaps[1].link = PRINTER_LINK_ONLINE;
    b->valid = true;
    b->gcode_state = BBL_GS_PAUSE;
    b->percent = 12;
    b->remaining_min = 315;
    b->layer = 8;
    b->total_layers = 400;
    b->nozzle = 180, b->nozzle_target = 0;
    b->bed = 55, b->bed_target = 55;
    b->speed_level = 3, b->speed_mag = 124;
    b->has_light = true;
    b->signature_required = true;
    strcpy(b->subtask, "Benchy");
    // 塔可的A1 实测：0、2 号有料盘但没设置类型；外挂 PETG
    b->ams_trays = 4;
    b->tray[0].present = b->tray[2].present = true;
    b->vt.loaded = b->vt.present = true;
    strcpy(b->vt.type, "PETG");
    b->vt.color = 0x5A657B;

    settings_init();
    ui_init();
    fake_camera_frame();
    fake_cover();
    run(600);
    shot("overview");

    tap(260, 300);   // 点一号机
    run(600);
    shot("detail_running");

    tap(544 + 20 + 220 + 104, 64 + 20 + 30);   // 停止按钮
    run(400);
    shot("detail_confirm_stop");

    // 第二轮：二号机详情（只读 + 命令被拒）以及一号机访问码错误
    lv_obj_t *mb = lv_obj_get_child(lv_layer_top(), -1);
    if (mb) lv_obj_delete(mb);   // 关掉确认框（模拟器里直接删）
    tap(70, 32);                 // 返回
    run(400);
    snaps[0].link = PRINTER_LINK_BUSY;
    tap(760, 300);               // 二号机
    run(600);
    b->result_seq = 1;
    strcpy(b->result_cmd, "resume");
    b->result_ok = false;
    strcpy(b->result_reason, "mqtt message verify failed");
    run(400);
    shot("detail_readonly");
    tap(70, 32);
    run(600);
    shot("overview_auth_failed");

    // 设置页
    tap(255, 32);
    run(400);
    shot("settings_display");
    tap(120, 104 + 66 * 1);
    run(300);
    shot("settings_time");
    tap(120, 104 + 66 * 3);
    run(300);
    shot("settings_printer");
    tap(600, 290);
    run(400);
    shot("settings_edit");
    return 0;
}
