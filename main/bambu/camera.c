#include "camera.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "printer.h"

static const char *TAG = "camera";

#define CAM_PORT 6000
#define JPEG_MAX (768 * 1024)               // 单帧 JPEG 上限
#define DEC_MAX_W 1920
#define DEC_MAX_H 1088
#define FRAME_BYTES (CAMERA_VIEW_W * CAMERA_VIEW_H * 2)
#define FRAME_ALLOC (((FRAME_BYTES) + 127) & ~127)
#define RETRY_FAST_MS 3000
#define RETRY_SLOW_MS 30000

extern const char bambu_ca_pem_start[] asm("_binary_bambu_ca_pem_start");
extern const char bambu_ca_pem_end[] asm("_binary_bambu_ca_pem_end");

typedef struct {
    int idx;
    volatile camera_state_t state;
    uint8_t *jpeg;            // 由 jpeg_alloc_decoder_mem 分配（DMA 对齐）
    uint8_t *frame[2];        // 缩放后的 RGB565，双缓冲
    int front;
    bool has_frame;
    bool logged;              // 已打印过分辨率
    lv_image_dsc_t dsc[2];
    lv_obj_t *imgs[CAMERA_MAX_BIND];
    int nimg;
} cam_t;

static cam_t s_cams[PRINTER_MAX];

// 解码器、PPA 和解码输出缓冲由所有打印机共用，用互斥锁串行
static SemaphoreHandle_t s_hw_lock;
static jpeg_decoder_handle_t s_dec;
static ppa_client_handle_t s_ppa;
static uint8_t *s_dec_out;
static size_t s_dec_out_size;

const char *camera_state_text(camera_state_t st)
{
    switch (st) {
    case CAMERA_OFF: return "摄像头未开启";
    case CAMERA_CONNECTING: return "正在连接摄像头…";
    case CAMERA_STREAMING: return "";
    case CAMERA_REJECTED: return "打印机拒绝了视频连接\n（被 Bambu Studio 占用，或访问码错误）";
    case CAMERA_CONN_FAILED: return "连不上摄像头，稍后重试\n（电脑上开着 Bambu Studio 的摄像头画面时会被占用）";
    case CAMERA_DECODE_FAILED: return "画面解码失败";
    default: return "";
    }
}

camera_state_t camera_state(int idx)
{
    return (idx >= 0 && idx < PRINTER_MAX) ? s_cams[idx].state : CAMERA_OFF;
}

bool camera_has_frame(int idx)
{
    return idx >= 0 && idx < PRINTER_MAX && s_cams[idx].has_frame;
}

void camera_bind_image(int idx, lv_obj_t *img)
{
    if (idx < 0 || idx >= PRINTER_MAX) return;
    cam_t *c = &s_cams[idx];
    if (c->nimg < CAMERA_MAX_BIND) c->imgs[c->nimg++] = img;
}

static bool hw_init(void)
{
    if (s_hw_lock) return true;
    s_hw_lock = xSemaphoreCreateMutex();
    jpeg_decode_engine_cfg_t ecfg = {.intr_priority = 0, .timeout_ms = 200};
    if (jpeg_new_decoder_engine(&ecfg, &s_dec) != ESP_OK) return false;
    jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    s_dec_out = jpeg_alloc_decoder_mem(DEC_MAX_W * DEC_MAX_H * 2, &out_cfg, &s_dec_out_size);
    if (!s_dec_out) return false;
    ppa_client_config_t pcfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    return ppa_register_client(&pcfg, &s_ppa) == ESP_OK;
}

// 读满 n 字节；返回 false 表示断开/超时
static bool read_exact(esp_tls_t *tls, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = esp_tls_conn_read(tls, buf + got, n - got);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

// 解码 + 缩放到 back 缓冲；成功返回 true
static bool decode_frame(cam_t *c, size_t len, uint8_t *dst)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(c->jpeg, len, &info) != ESP_OK) return false;
    if (info.width == 0 || info.width > DEC_MAX_W || info.height > DEC_MAX_H) return false;
    uint32_t aligned_h = (info.height + 15) & ~15;

    bool ok = false;
    xSemaphoreTake(s_hw_lock, portMAX_DELAY);
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_len = 0;
    if (jpeg_decoder_process(s_dec, &dcfg, c->jpeg, len, s_dec_out, s_dec_out_size, &out_len) == ESP_OK) {
        // 等比缩放后裁掉多出的边，铺满 480x270（PPA 缩放步进 1/16，向上取整保证铺满）
        if (!c->logged) {
            c->logged = true;
            ESP_LOGI(TAG, "[%d] camera frame %lux%lu", c->idx, (unsigned long)info.width, (unsigned long)info.height);
        }
        float sx = (float)CAMERA_VIEW_W / info.width, sy = (float)CAMERA_VIEW_H / info.height;
        float s = sx > sy ? sx : sy;
        int s16 = (int)(s * 16);
        if ((float)s16 < s * 16) s16++;
        if (s16 < 1) s16 = 1;
        s = s16 / 16.0f;
        uint32_t bw = (uint32_t)(CAMERA_VIEW_W / s), bh = (uint32_t)(CAMERA_VIEW_H / s);
        if (bw > info.width) bw = info.width;
        if (bh > info.height) bh = info.height;
        uint32_t ow = (uint32_t)(bw * s), oh = (uint32_t)(bh * s);
        if (ow > CAMERA_VIEW_W) ow = CAMERA_VIEW_W;
        if (oh > CAMERA_VIEW_H) oh = CAMERA_VIEW_H;
        ppa_srm_oper_config_t op = {
            .in = {.buffer = s_dec_out, .pic_w = info.width, .pic_h = aligned_h, .block_w = bw,
                   .block_h = bh, .block_offset_x = (info.width - bw) / 2, .block_offset_y = (info.height - bh) / 2,
                   .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out = {.buffer = dst, .buffer_size = FRAME_ALLOC, .pic_w = CAMERA_VIEW_W, .pic_h = CAMERA_VIEW_H,
                    .block_offset_x = (CAMERA_VIEW_W - ow) / 2, .block_offset_y = (CAMERA_VIEW_H - oh) / 2,
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = s,
            .scale_y = s,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        ok = ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
    }
    xSemaphoreGive(s_hw_lock);
    return ok;
}

static void present(cam_t *c)
{
    if (!lvgl_port_lock(1000)) return;
    c->front ^= 1;
    lv_image_dsc_t *d = &c->dsc[c->front];
    lv_image_cache_drop(d);
    for (int i = 0; i < c->nimg; i++) {
        lv_image_set_src(c->imgs[i], d);
        lv_obj_invalidate(c->imgs[i]);
    }
    c->has_frame = true;
    lvgl_port_unlock();
}

static void build_auth(uint8_t out[80], const char *code)
{
    memset(out, 0, 80);
    out[0] = 0x40;            // 0x40
    out[5] = 0x30;            // 0x3000 小端
    memcpy(out + 16, "bblp", 4);
    size_t n = strlen(code);
    memcpy(out + 48, code, n > 32 ? 32 : n);
}

static void cam_task(void *arg)
{
    cam_t *c = arg;
    const printer_cfg_t *cfg = printer_cfg(c->idx);
    int fails = 0;
    uint8_t hdr[16], auth[80];
    build_auth(auth, cfg->access_code);

    // 等 MQTT 先连上（最多 20 秒），避免几路 TLS 同时握手；打印机同时处理多个握手时容易超时
    printer_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM);
    for (int i = 0; snap && i < 40; i++) {
        if (printer_get(c->idx, snap) && snap->link == PRINTER_LINK_ONLINE) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    free(snap);

    for (;;) {
        c->state = CAMERA_CONNECTING;
        esp_tls_cfg_t tcfg = {.timeout_ms = 10000};
#if CONFIG_BBL_TLS_VERIFY
        tcfg.cacert_buf = (const unsigned char *)bambu_ca_pem_start;
        tcfg.cacert_bytes = bambu_ca_pem_end - bambu_ca_pem_start;
        tcfg.common_name = cfg->serial;
#else
        tcfg.skip_common_name = true;
#endif
        esp_tls_t *tls = esp_tls_init();
        bool got_any = false;
        if (tls && esp_tls_conn_new_sync(cfg->host, strlen(cfg->host), CAM_PORT, &tcfg, tls) == 1) {
            int fd = -1;
            if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0) {
                struct timeval tv = {.tv_sec = 20};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            }
            ESP_LOGI(TAG, "[%s] connected, sending auth", cfg->name);
            if (esp_tls_conn_write(tls, auth, sizeof(auth)) == sizeof(auth)) {
                while (read_exact(tls, hdr, sizeof(hdr))) {
                    uint32_t size = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
                    if (size < 4 || size > JPEG_MAX) {
                        ESP_LOGW(TAG, "[%s] bad frame size %lu", cfg->name, (unsigned long)size);
                        break;
                    }
                    if (!read_exact(tls, c->jpeg, size)) break;
                    got_any = true;
                    fails = 0;
                    if (c->jpeg[0] != 0xFF || c->jpeg[1] != 0xD8) continue;
                    if (decode_frame(c, size, c->frame[c->front ^ 1])) {
                        c->state = CAMERA_STREAMING;
                        present(c);
                    } else {
                        c->state = CAMERA_DECODE_FAILED;
                    }
                }
            }
            // 连上了但一帧都没有：通常是认证被拒（访问码/开发者模式）
            if (!got_any) c->state = CAMERA_REJECTED;
        } else {
            c->state = CAMERA_CONN_FAILED;
        }
        if (tls) esp_tls_conn_destroy(tls);
        ESP_LOGW(TAG, "[%s] stream ended (%s)", cfg->name, got_any ? "after frames" : "no frames");
        fails++;
        vTaskDelay(pdMS_TO_TICKS(fails > 5 ? RETRY_SLOW_MS : RETRY_FAST_MS));
    }
}

void camera_start_all(void)
{
    static bool started;
    if (started) return;
    started = true;
    if (!hw_init()) {
        ESP_LOGE(TAG, "JPEG/PPA init failed, camera disabled");
        return;
    }
    jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    for (int i = 0; i < printers_count(); i++) {
        printer_snapshot_t snap;
        if (!printer_get(i, &snap) || snap.link == PRINTER_LINK_NOT_CONFIGURED) continue;
        cam_t *c = &s_cams[i];
        c->idx = i;
        size_t sz;
        c->jpeg = jpeg_alloc_decoder_mem(JPEG_MAX, &in_cfg, &sz);
        for (int b = 0; b < 2; b++) {
            c->frame[b] = heap_caps_aligned_calloc(128, 1, FRAME_ALLOC, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            c->dsc[b] = (lv_image_dsc_t){
                .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                           .w = CAMERA_VIEW_W, .h = CAMERA_VIEW_H, .stride = CAMERA_VIEW_W * 2},
                .data_size = FRAME_BYTES,
                .data = c->frame[b],
            };
        }
        if (!c->jpeg || !c->frame[0] || !c->frame[1]) {
            ESP_LOGE(TAG, "no memory for camera %d", i);
            continue;
        }
        xTaskCreatePinnedToCore(cam_task, "cam", 6144, c, 3, NULL, 0);
    }
}
