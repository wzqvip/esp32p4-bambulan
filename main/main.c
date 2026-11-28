// ESP32-P4 拓竹打印机局域网状态/控制屏
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "board.h"
#include "camera.h"
#include "cover.h"
#include "discover.h"
#include "net.h"
#include "printer.h"
#include "settings.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "main";

static void on_network_ready(void)
{
    printers_start();
#if !CONFIG_BBL_DEBUG_NO_CAMERA
    camera_start_all();
#endif
    cover_start_all();
    discover_start();
}

// mbedTLS 的硬件 AES / RSA 中断是第一次用到时才分配，分在当时所在的那个核上，分不到就直接 abort。
// TLS 连接跑在 core 1 时，core 1 的中断输入可能已经被其他外设占满（实测崩过一次）。
// 开机先在 app_main（core 0）里各用一次，让这两个中断分到 core 0。
static void crypto_irq_warmup(void)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);   // 开了 MBEDTLS_AES_USE_INTERRUPT 时在这里分配 AES 中断
    mbedtls_aes_free(&aes);

    mbedtls_mpi a, e, n, x;
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&x);
    if (mbedtls_mpi_lset(&a, 3) == 0 && mbedtls_mpi_lset(&e, 65537) == 0 && mbedtls_mpi_lset(&n, 1000003) == 0)
        mbedtls_mpi_exp_mod(&x, &a, &e, &n, NULL);   // 硬件 RSA 加速器，第一次用时分配中断
    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&x);
}

void app_main(void)
{
    crypto_irq_warmup();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_init();

    ESP_ERROR_CHECK(printers_init());

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));
    if (lvgl_port_lock(0)) {
        ui_init();
        ui_debug_init(disp);
        ui_stats_init(disp);
        lvgl_port_unlock();
    }
    board_set_backlight(settings_get()->brightness);

    if (net_start(on_network_ready) != ESP_OK) {
        ESP_LOGE(TAG, "network not started, check Wi-Fi settings in menuconfig");
    }
}
