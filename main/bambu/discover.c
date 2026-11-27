// 监听拓竹打印机的局域网 SSDP 广播（UDP 2021 / 1990，组播 239.255.255.250），
// 按 USN（序列号）取出 DevName.bambu.com（打印机在 App 里的名字，实测是 UTF-8）显示在屏幕上。
#include "discover.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "printer.h"

static const char *TAG = "discover";

extern const uint16_t gbk_table_count;
extern const uint16_t gbk_table[][2];

// GBK -> UTF-8；查不到的双字节字符跳过
static void gbk_to_utf8(const char *in, size_t len, char *out, size_t cap)
{
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)in, *end = p + len;
    while (p < end && n + 4 < cap) {
        if (*p < 0x80) {
            out[n++] = (char)*p++;
            continue;
        }
        if (p + 1 >= end) break;
        uint16_t code = p[0] << 8 | p[1];
        p += 2;
        uint32_t cp = 0;
        for (int i = 0; i < gbk_table_count; i++) {   // 表按 Unicode 排序，这里反查只能线性找；名字很短，无所谓
            if (gbk_table[i][1] == code) {
                cp = gbk_table[i][0];
                break;
            }
        }
        if (!cp) continue;
        if (cp < 0x800) {
            out[n++] = (char)(0xC0 | cp >> 6);
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | cp >> 12);
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = '\0';
}

static bool valid_utf8(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len;) {
        int n = p[i] < 0x80 ? 0 : (p[i] & 0xE0) == 0xC0 ? 1 : (p[i] & 0xF0) == 0xE0 ? 2 : (p[i] & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= len + (n == 0)) return false;
        for (int k = 1; k <= n; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

// 在报文里找 "Key: value"（不区分大小写），返回 value 起点和长度
static const char *header(const char *msg, const char *key, size_t *len)
{
    size_t kl = strlen(key);
    for (const char *line = msg; line && *line;) {
        if (strncasecmp(line, key, kl) == 0 && line[kl] == ':') {
            const char *v = line + kl + 1;
            while (*v == ' ') v++;
            const char *e = v;
            while (*e && *e != '\r' && *e != '\n') e++;
            *len = e - v;
            return v;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

static void handle_packet(char *buf)
{
    size_t ul, nl;
    const char *usn = header(buf, "USN", &ul);
    const char *name = header(buf, "DevName.bambu.com", &nl);
    if (!usn || !name || ul == 0 || ul >= 24 || nl == 0) return;
    char serial[24], utf8[48];
    memcpy(serial, usn, ul);
    serial[ul] = '\0';
    if (valid_utf8((const uint8_t *)name, nl)) {
        // 截断时别切在多字节字符中间
        size_t n = nl < sizeof(utf8) - 1 ? nl : sizeof(utf8) - 1;
        while (n > 0 && n < nl && ((uint8_t)name[n] & 0xC0) == 0x80) n--;
        memcpy(utf8, name, n);
        utf8[n] = '\0';
    } else {
        gbk_to_utf8(name, nl, utf8, sizeof(utf8));   // 兜底：老固件可能是 GBK
    }
    if (utf8[0]) printer_set_dev_name(serial, utf8);
}

static int open_socket(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    struct ip_mreq m = {.imr_multiaddr.s_addr = inet_addr("239.255.255.250"), .imr_interface.s_addr = htonl(INADDR_ANY)};
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));   // 失败也没关系，广播包照样能收
    return s;
}

static void discover_task(void *arg)
{
    int socks[2] = {open_socket(2021), open_socket(1990)};
    ESP_LOGI(TAG, "listening for printer broadcasts (2021:%s 1990:%s)", socks[0] >= 0 ? "ok" : "fail",
             socks[1] >= 0 ? "ok" : "fail");
    static char buf[1024];
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        for (int i = 0; i < 2; i++) {
            if (socks[i] >= 0) {
                FD_SET(socks[i], &rf);
                if (socks[i] > maxfd) maxfd = socks[i];
            }
        }
        if (maxfd < 0) break;
        if (select(maxfd + 1, &rf, NULL, NULL, NULL) <= 0) continue;
        for (int i = 0; i < 2; i++) {
            if (socks[i] < 0 || !FD_ISSET(socks[i], &rf)) continue;
            int n = recv(socks[i], buf, sizeof(buf) - 1, 0);
            if (n <= 0) continue;
            buf[n] = '\0';
            handle_packet(buf);
        }
    }
    vTaskDelete(NULL);
}

void discover_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    xTaskCreatePinnedToCore(discover_task, "discover", 4096, NULL, 2, NULL, 0);
}
