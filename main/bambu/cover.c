#include "cover.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "src/libs/lodepng/lodepng.h"

#include "printer.h"

static const char *TAG = "cover";

#define FTP_PORT 990
#define TAIL_LEN (64 * 1024)          // 读 3mf 末尾这么多字节找 ZIP 目录
#define PNG_MAX (2 * 1024 * 1024)
#define FILE_MAX (16 * 1024 * 1024)       // 3mf 超过这个大小就不取预览图
#define POLL_MS 3000
#define RETRY_MS 60000
#define MAX_TRIES 3

extern const char bambu_ca_pem_start[] asm("_binary_bambu_ca_pem_start");
extern const char bambu_ca_pem_end[] asm("_binary_bambu_ca_pem_end");

typedef struct {
    int idx;
    lv_obj_t *imgs[COVER_MAX_BIND];
    int nimg;
    uint8_t *buf[2];                  // ARGB8888，双缓冲
    lv_image_dsc_t dsc[2];
    int front;
    volatile bool has;
} cover_t;

static cover_t s_cov[PRINTER_MAX];

void cover_bind_image(int idx, lv_obj_t *img)
{
    if (idx < 0 || idx >= PRINTER_MAX) return;
    cover_t *c = &s_cov[idx];
    if (c->nimg < COVER_MAX_BIND) c->imgs[c->nimg++] = img;
}

bool cover_has_image(int idx)
{
    return idx >= 0 && idx < PRINTER_MAX && s_cov[idx].has;
}

// ================= FTPS 客户端（最小实现） =================
typedef struct {
    const printer_cfg_t *cfg;
    esp_tls_t *ctrl;
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    esp_tls_client_session_t *sess;   // 数据连接要复用控制连接的 TLS 会话（vsftpd 的 require_ssl_reuse）
#endif
    char line[512];
} ftp_t;

static void tls_cfg_init(const printer_cfg_t *cfg, esp_tls_cfg_t *t)
{
    memset(t, 0, sizeof(*t));
    t->timeout_ms = 10000;
#if CONFIG_BBL_TLS_VERIFY
    t->cacert_buf = (const unsigned char *)bambu_ca_pem_start;
    t->cacert_bytes = bambu_ca_pem_end - bambu_ca_pem_start;
    t->common_name = cfg->serial;
#else
    t->skip_common_name = true;
#endif
}

static esp_tls_t *tls_open(const printer_cfg_t *cfg, int port, esp_tls_cfg_t *t)
{
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return NULL;
    if (esp_tls_conn_new_sync(cfg->host, strlen(cfg->host), port, t, tls) != 1) {
        esp_tls_conn_destroy(tls);
        return NULL;
    }
    return tls;
}

static bool tls_write_all(esp_tls_t *tls, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        int w = esp_tls_conn_write(tls, p, n);
        if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

// 读一行（去掉 \r\n），返回 false 表示断开
static bool ftp_getline(ftp_t *f)
{
    size_t n = 0;
    for (;;) {
        char ch;
        int r = esp_tls_conn_read(f->ctrl, &ch, 1);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        if (ch == '\n') break;
        if (ch != '\r' && n < sizeof(f->line) - 1) f->line[n++] = ch;
    }
    f->line[n] = '\0';
    return true;
}

// 读完整应答（含多行 "ddd-..." 续行），返回三位应答码，-1 表示断开
static int ftp_resp(ftp_t *f)
{
    if (!ftp_getline(f)) return -1;
    if (strlen(f->line) < 3) return -1;
    int code = atoi(f->line);
    if (f->line[3] == '-') {
        char end[5];
        snprintf(end, sizeof(end), "%d ", code);
        do {
            if (!ftp_getline(f)) return -1;
        } while (strncmp(f->line, end, 4) != 0);
    }
    return code;
}

static int ftp_cmd(ftp_t *f, const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf) - 2) return -1;
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (!tls_write_all(f->ctrl, buf, n)) return -1;
    return ftp_resp(f);
}

static void ftp_close(ftp_t *f)
{
    if (f->ctrl) {
        tls_write_all(f->ctrl, "QUIT\r\n", 6);
        esp_tls_conn_destroy(f->ctrl);
        f->ctrl = NULL;
    }
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    if (f->sess) {
        esp_tls_free_client_session(f->sess);
        f->sess = NULL;
    }
#endif
}

static bool ftp_open(ftp_t *f, const printer_cfg_t *cfg)
{
    memset(f, 0, sizeof(*f));
    f->cfg = cfg;
    esp_tls_cfg_t t;
    tls_cfg_init(cfg, &t);
    f->ctrl = tls_open(cfg, FTP_PORT, &t);
    if (!f->ctrl) {
        ESP_LOGW(TAG, "[%s] FTPS connect failed", cfg->name);
        return false;
    }
    int c = ftp_resp(f);
    if (c != 220) goto fail;
    c = ftp_cmd(f, "USER bblp");
    if (c != 331 && c != 230) goto fail;
    if (c == 331 && ftp_cmd(f, "PASS %s", cfg->access_code) != 230) {
        ESP_LOGW(TAG, "[%s] FTP login rejected: %s", cfg->name, f->line);
        goto fail_quiet;
    }
    if (ftp_cmd(f, "PBSZ 0") != 200 || ftp_cmd(f, "PROT P") != 200 || ftp_cmd(f, "TYPE I") != 200) goto fail;
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    f->sess = esp_tls_get_client_session(f->ctrl);
#endif
    return true;
fail:
    ESP_LOGW(TAG, "[%s] FTP unexpected reply: %s", cfg->name, f->line);
fail_quiet:
    ftp_close(f);
    return false;
}

// SIZE：文件存在返回字节数，否则 -1
static long ftp_size(ftp_t *f, const char *path)
{
    if (ftp_cmd(f, "SIZE %s", path) != 213) return -1;
    return strtol(f->line + 4, NULL, 10);
}

// 读文件 [off, off+len) 到 buf，返回实际读到的字节数，-1 表示失败（控制连接可能已不可用）
static int ftp_read_range(ftp_t *f, const char *path, long off, size_t len, uint8_t *buf)
{
    int rc;
#define FAIL_IF(cond, what)                                                                  \
    if (cond) {                                                                              \
        ESP_LOGW(TAG, "[%s] %s failed: %s", f->cfg->name, what, f->line);                    \
        return -1;                                                                           \
    }
    rc = ftp_cmd(f, "PASV");
    FAIL_IF(rc != 227, "PASV");
    const char *p = strchr(f->line, '(');
    int h[6];
    if (!p || sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6) {
        ESP_LOGW(TAG, "[%s] bad PASV reply: %s", f->cfg->name, f->line);
        return -1;
    }
    int port = h[4] * 256 + h[5];
    if (off > 0) {
        rc = ftp_cmd(f, "REST %ld", off);
        FAIL_IF(rc != 350, "REST");
    }

    // 先发 RETR，再连数据端口：服务器接受连接、回 150 之后才开始数据通道的 TLS 握手
    char cmd[400];
    int n = snprintf(cmd, sizeof(cmd), "RETR %s\r\n", path);
    FAIL_IF(n >= (int)sizeof(cmd) || !tls_write_all(f->ctrl, cmd, n), "send RETR");

    esp_tls_cfg_t t;
    tls_cfg_init(f->cfg, &t);
#ifdef CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    t.client_session = f->sess;
#endif
    esp_tls_t *data = tls_open(f->cfg, port, &t);   // 主机用配置里的 IP，不信任 PASV 里的地址
    int c = ftp_resp(f);
    if (!data || (c != 150 && c != 125)) {
        ESP_LOGW(TAG, "[%s] RETR failed (%d): %s", f->cfg->name, c, f->line);
        if (data) esp_tls_conn_destroy(data);
        return -1;
    }
    size_t got = 0;
    while (got < len) {
        int r = esp_tls_conn_read(data, buf + got, len - got);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;
        got += r;
    }
    esp_tls_conn_destroy(data);
    ESP_LOGI(TAG, "[%s] RETR @%ld: got %u/%u bytes", f->cfg->name, off, (unsigned)got, (unsigned)len);
    // 提前关闭数据连接时服务器回 426/451，读完到 EOF 时回 226，都算正常
    c = ftp_resp(f);
    if (c < 0) ESP_LOGW(TAG, "[%s] no transfer reply", f->cfg->name);   // 数据已读完就不算失败
    else ESP_LOGI(TAG, "[%s] transfer reply: %s", f->cfg->name, f->line);
#undef FAIL_IF
    return (int)got;
}

// ================= ZIP（只解析需要的部分） =================
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

typedef struct {
    uint64_t local_off, comp, uncomp;
    uint16_t method;
} zip_entry_t;

// 在中央目录里找 name；找不到再退而找任意 Metadata/plate_<数字>.png
static bool zip_find(const uint8_t *cd, size_t cd_len, const char *name, zip_entry_t *out)
{
    bool found_fallback = false;
    size_t nlen = strlen(name);
    for (size_t i = 0; i + 46 <= cd_len && rd32(cd + i) == 0x02014b50;) {
        const uint8_t *e = cd + i;
        uint16_t n = rd16(e + 28), m = rd16(e + 30), k = rd16(e + 32);
        if (i + 46 + n + m + k > cd_len) break;
        const char *fn = (const char *)e + 46;
        bool exact = n == nlen && memcmp(fn, name, n) == 0;
        bool fallback = false;
        if (!exact && !found_fallback && n > 19 && memcmp(fn, "Metadata/plate_", 15) == 0 &&
            memcmp(fn + n - 4, ".png", 4) == 0) {
            fallback = true;
            for (int j = 15; j < n - 4; j++) {
                if (fn[j] < '0' || fn[j] > '9') fallback = false;
            }
        }
        if (exact || fallback) {
            zip_entry_t z = {.method = rd16(e + 10), .comp = rd32(e + 20), .uncomp = rd32(e + 24),
                             .local_off = rd32(e + 42)};
            // zip64 扩展字段：被置成 0xFFFFFFFF 的值按 原始大小/压缩大小/偏移 的顺序出现
            const uint8_t *x = e + 46 + n, *xe = x + m;
            while (x + 4 <= xe) {
                uint16_t id = rd16(x), sz = rd16(x + 2);
                if (id == 0x0001) {
                    const uint8_t *v = x + 4;
                    if (z.uncomp == 0xFFFFFFFF && v + 8 <= x + 4 + sz) z.uncomp = rd64(v), v += 8;
                    if (z.comp == 0xFFFFFFFF && v + 8 <= x + 4 + sz) z.comp = rd64(v), v += 8;
                    if (z.local_off == 0xFFFFFFFF && v + 8 <= x + 4 + sz) z.local_off = rd64(v);
                }
                x += 4 + sz;
            }
            *out = z;
            if (exact) return true;
            found_fallback = true;
        }
        i += 46 + n + m + k;
    }
    return found_fallback;
}

// 在文件尾部缓冲里定位中央目录：返回其在文件中的偏移和长度
static bool zip_locate_cd(const uint8_t *tail, size_t tail_len, long tail_off, uint64_t *cd_off, uint64_t *cd_size)
{
    if (tail_len < 22) return false;
    for (long i = (long)tail_len - 22; i >= 0; i--) {
        if (rd32(tail + i) != 0x06054b50) continue;
        *cd_size = rd32(tail + i + 12);
        *cd_off = rd32(tail + i + 16);
        if (*cd_off == 0xFFFFFFFF || *cd_size == 0xFFFFFFFF) {
            // zip64：前面紧挨着 20 字节的 locator，指向 zip64 结尾记录
            if (i < 20 || rd32(tail + i - 20) != 0x07064b50) return false;
            uint64_t z64 = rd64(tail + i - 20 + 8);
            if ((long)z64 < tail_off || z64 + 56 > (uint64_t)tail_off + tail_len) return false;
            const uint8_t *r = tail + (z64 - tail_off);
            if (rd32(r) != 0x06064b50) return false;
            *cd_size = rd64(r + 40);
            *cd_off = rd64(r + 48);
        }
        return true;
    }
    return false;
}

// ================= 取图 =================
// 从 gcode_file（例如 /data/Metadata/plate_3.gcode）里取盘号，取不到就是 1
static int plate_index(const bbl_status_t *st)
{
    const char *p = strstr(st->gcode_file, "plate_");
    int n = p ? atoi(p + 6) : 0;
    return n > 0 ? n : 1;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

extern const uint16_t gbk_table_count;
extern const uint16_t gbk_table[][2];

// UTF-8 -> GBK（打印机存储卡上的中文文件名是 GBK）。有无法转换的字符返回 false
static bool utf8_to_gbk(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) cp = (p[0] & 0x1F) << 6 | (p[1] & 0x3F), p += 2;
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) cp = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F), p += 3;
        else return false;
        if (cp < 0x80) {
            if (n + 1 >= cap) return false;
            out[n++] = (char)cp;
            continue;
        }
        int lo = 0, hi = gbk_table_count - 1, hit = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (gbk_table[mid][0] == cp) { hit = mid; break; }
            if (gbk_table[mid][0] < cp) lo = mid + 1;
            else hi = mid - 1;
        }
        if (hit < 0 || n + 2 >= cap) return false;
        out[n++] = (char)(gbk_table[hit][1] >> 8);
        out[n++] = (char)(gbk_table[hit][1] & 0xFF);
    }
    out[n] = '\0';
    return true;
}

static bool has_non_ascii(const char *s)
{
    for (; *s; s++) {
        if ((uint8_t)*s >= 0x80) return true;
    }
    return false;
}

// 按 ha-bambulab 的经验列出可能的 3mf 路径，返回第一个存在的
static bool find_3mf(ftp_t *f, const bbl_status_t *st, char *path, size_t cap, long *size)
{
    char cand[6][160];
    int nc = 0;
    if (ends_with(st->gcode_file, ".3mf")) {
        const char *base = strrchr(st->gcode_file, '/');
        base = base ? base + 1 : st->gcode_file;
        snprintf(cand[nc++], sizeof(cand[0]), "/cache/%s", base);
        snprintf(cand[nc++], sizeof(cand[0]), "/%s", st->gcode_file[0] == '/' ? st->gcode_file + 1 : st->gcode_file);
    }
    if (st->subtask[0] && !strchr(st->subtask, '/')) {
        const char *fmt[] = {"/cache/%s.3mf", "/cache/%s.gcode.3mf", "/%s.3mf", "/%s.gcode.3mf"};
        for (int i = 0; i < 4 && nc < 6; i++) {
            if (ends_with(st->subtask, ".3mf")) {
                if (i >= 2) break;
                snprintf(cand[nc++], sizeof(cand[0]), i == 0 ? "/cache/%s" : "/%s", st->subtask);
            } else {
                snprintf(cand[nc++], sizeof(cand[0]), fmt[i], st->subtask);
            }
        }
    }
    for (int i = 0; i < nc; i++) {
        long s = ftp_size(f, cand[i]);
        if (s > 0) {
            strlcpy(path, cand[i], cap);
            *size = s;
            return true;
        }
    }
    // 中文文件名：存储卡上是 GBK，再按 GBK 试一遍
    for (int i = 0; i < nc; i++) {
        char gbk[200];
        if (!has_non_ascii(cand[i]) || !utf8_to_gbk(cand[i], gbk, sizeof(gbk))) continue;
        long s = ftp_size(f, gbk);
        if (s > 0) {
            strlcpy(path, gbk, cap);
            *size = s;
            ESP_LOGI(TAG, "[%s] found GBK name for %s", f->cfg->name, cand[i]);
            return true;
        }
    }
    return false;
}

// PNG(RGBA) 缩放到 COVER_SIZE 见方（等比，盒式滤波），写成 LVGL 的 ARGB8888（内存顺序 B G R A）
static void scale_rgba(const uint8_t *src, unsigned w, unsigned h, uint8_t *dst)
{
    memset(dst, 0, COVER_SIZE * COVER_SIZE * 4);
    unsigned big = w > h ? w : h;
    unsigned ow = w * COVER_SIZE / big, oh = h * COVER_SIZE / big;
    unsigned ox = (COVER_SIZE - ow) / 2, oy = (COVER_SIZE - oh) / 2;
    for (unsigned y = 0; y < oh; y++) {
        unsigned y0 = y * h / oh, y1 = (y + 1) * h / oh;
        if (y1 <= y0) y1 = y0 + 1;
        for (unsigned x = 0; x < ow; x++) {
            unsigned x0 = x * w / ow, x1 = (x + 1) * w / ow;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (unsigned sy = y0; sy < y1; sy++) {
                const uint8_t *row = src + (size_t)sy * w * 4;
                for (unsigned sx = x0; sx < x1; sx++) {
                    const uint8_t *px = row + sx * 4;
                    // 按 alpha 加权，避免透明区域的颜色渗进边缘
                    r += px[0] * px[3], g += px[1] * px[3], b += px[2] * px[3], a += px[3];
                    cnt++;
                }
            }
            uint8_t *o = dst + ((size_t)(oy + y) * COVER_SIZE + ox + x) * 4;
            if (a) {
                o[0] = b / a, o[1] = g / a, o[2] = r / a;
            }
            o[3] = a / cnt;
        }
    }
}

static void present(cover_t *c, bool show)
{
    if (!lvgl_port_lock(1000)) return;
    if (show) {
        c->front ^= 1;
        lv_image_cache_drop(&c->dsc[c->front]);
    }
    for (int i = 0; i < c->nimg; i++) lv_image_set_src(c->imgs[i], show ? &c->dsc[c->front] : NULL);
    c->has = show;
    lvgl_port_unlock();
}

static bool fetch_cover(cover_t *c, const bbl_status_t *st)
{
    const printer_cfg_t *cfg = printer_cfg(c->idx);
    ftp_t f;
    if (!ftp_open(&f, cfg)) return false;

    bool ok = false;
    uint8_t *file = NULL, *png = NULL, *rgba = NULL;
    char path[200];
    long size = 0;
    if (!find_3mf(&f, st, path, sizeof(path), &size)) {
        ESP_LOGW(TAG, "[%s] 3mf not found for '%s' / '%s'", cfg->name, st->subtask, st->gcode_file);
        goto out;
    }
    if (size > FILE_MAX) {
        ESP_LOGW(TAG, "[%s] %s is %ld bytes, too large for preview", cfg->name, path, size);
        goto out;
    }
    // 打印机的 FTP 不支持 REST（回 502），也不能中途断开数据连接（会连控制连接一起断），
    // 所以整个 3mf 读进 PSRAM 再找 ZIP 目录
    ESP_LOGI(TAG, "[%s] reading %s (%ld bytes)", cfg->name, path, size);
    file = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!file) goto out;
    int64_t t0 = esp_timer_get_time();
    int got = ftp_read_range(&f, path, 0, size, file);
    if (got != size) {
        ESP_LOGW(TAG, "[%s] read %d/%ld", cfg->name, got, size);
        goto out;
    }
    ESP_LOGI(TAG, "[%s] downloaded in %lld ms", cfg->name, (esp_timer_get_time() - t0) / 1000);

    uint64_t cd_off, cd_size;
    long tail_off = size > TAIL_LEN ? size - TAIL_LEN : 0;
    if (!zip_locate_cd(file + tail_off, size - tail_off, tail_off, &cd_off, &cd_size) ||
        cd_off + cd_size > (uint64_t)size) {
        ESP_LOGW(TAG, "[%s] ZIP directory not found", cfg->name);
        goto out;
    }
    char want[40];
    snprintf(want, sizeof(want), "Metadata/plate_%d.png", plate_index(st));
    zip_entry_t z;
    if (!zip_find(file + cd_off, cd_size, want, &z)) {
        ESP_LOGW(TAG, "[%s] no plate preview in 3mf", cfg->name);
        goto out;
    }
    ESP_LOGI(TAG, "[%s] entry: method %u comp %llu uncomp %llu at %llu", cfg->name, z.method,
             (unsigned long long)z.comp, (unsigned long long)z.uncomp, (unsigned long long)z.local_off);
    if (z.comp == 0 || z.comp > PNG_MAX || z.uncomp > PNG_MAX || (z.method != 0 && z.method != 8)) goto out;
    if (z.local_off + 30 > (uint64_t)size || rd32(file + z.local_off) != 0x04034b50) {
        ESP_LOGW(TAG, "[%s] bad local header", cfg->name);
        goto out;
    }
    const uint8_t *lh = file + z.local_off;
    uint64_t data_off = z.local_off + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data_off + z.comp > (uint64_t)size) {
        ESP_LOGW(TAG, "[%s] entry truncated", cfg->name);
        goto out;
    }

    const uint8_t *png_data = file + data_off;
    size_t png_len = z.comp;
    if (z.method == 8) {
        size_t outsize = 0;
        unsigned e = lodepng_inflate(&png, &outsize, png_data, z.comp, &lodepng_default_decompress_settings);
        if (e) {
            ESP_LOGW(TAG, "[%s] inflate failed: %u", cfg->name, e);
            goto out;
        }
        png_data = png;
        png_len = outsize;
    }
    unsigned w, h;
    if (lodepng_decode32(&rgba, &w, &h, png_data, png_len) != 0 || !w || !h) {
        ESP_LOGW(TAG, "[%s] PNG decode failed", cfg->name);
        goto out;
    }
    scale_rgba(rgba, w, h, c->buf[c->front ^ 1]);
    ESP_LOGI(TAG, "[%s] preview %ux%u ready", cfg->name, w, h);
    ok = true;
out:
    ftp_close(&f);
    free(file);
    lv_free(png);
    lv_free(rgba);
    if (ok) present(c, true);
    return ok;
}

static void cover_task(void *arg)
{
    cover_t *c = arg;
    char done_key[200] = "", try_key[200] = "";
    int tries = 0;
    int64_t next_try = 0;
    printer_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) vTaskDelete(NULL);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (!printer_get(c->idx, s) || s->link != PRINTER_LINK_ONLINE || !s->st.valid) continue;
        const bbl_status_t *st = &s->st;
        char key[200];
        if (st->gcode_state == BBL_GS_IDLE || (!st->subtask[0] && !st->gcode_file[0])) key[0] = '\0';
        else snprintf(key, sizeof(key), "%s|%s", st->subtask, st->gcode_file);

        if (strcmp(key, done_key) == 0) continue;
        if (c->has) present(c, false);   // 任务变了，先撤掉旧图
        if (!key[0]) {
            done_key[0] = '\0';
            continue;
        }
        if (strcmp(key, try_key) != 0) {
            snprintf(try_key, sizeof(try_key), "%s", key);
            tries = 0;
            next_try = 0;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (tries >= MAX_TRIES || now < next_try) continue;
        tries++;
        if (fetch_cover(c, st)) {
            snprintf(done_key, sizeof(done_key), "%s", key);
        } else {
            next_try = now + RETRY_MS;
        }
    }
}

void cover_start_all(void)
{
    static bool started;
    if (started) return;
    started = true;
    for (int i = 0; i < printers_count(); i++) {
        printer_snapshot_t snap;
        if (!printer_get(i, &snap) || snap.link == PRINTER_LINK_NOT_CONFIGURED) continue;
        cover_t *c = &s_cov[i];
        c->idx = i;
        for (int b = 0; b < 2; b++) {
            c->buf[b] = heap_caps_calloc(1, COVER_SIZE * COVER_SIZE * 4, MALLOC_CAP_SPIRAM);
            c->dsc[b] = (lv_image_dsc_t){
                .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                           .w = COVER_SIZE, .h = COVER_SIZE, .stride = COVER_SIZE * 4},
                .data_size = COVER_SIZE * COVER_SIZE * 4,
                .data = c->buf[b],
            };
        }
        if (!c->buf[0] || !c->buf[1]) {
            ESP_LOGE(TAG, "no memory for cover %d", i);
            continue;
        }
        xTaskCreatePinnedToCore(cover_task, "cover", 8192, c, 2, NULL, 0);
    }
}
