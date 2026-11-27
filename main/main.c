// ESP32-P4 拓竹打印机局域网状态/控制屏
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "board.h"
#include "camera.h"
#include "cover.h"
#include "discover.h"
#include "net.h"
#include "printer.h"
#include "ui.h"

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

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(printers_init());

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));
    if (lvgl_port_lock(0)) {
        ui_init();
        ui_debug_init(disp);
        lvgl_port_unlock();
    }
    board_set_backlight(CONFIG_BBL_BACKLIGHT_PERCENT);

    if (net_start(on_network_ready) != ESP_OK) {
        ESP_LOGE(TAG, "network not started, check Wi-Fi settings in menuconfig");
    }
}
