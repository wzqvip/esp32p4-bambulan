#include "board.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

#if CONFIG_BBL_LCD_SWAP_BYTES
#define LCD_SWAP_BYTES 1
#else
#define LCD_SWAP_BYTES 0
#endif

// 引脚来自 ELECROW 官方例程（V1.0~V1.2 相同）
#define PIN_BACKLIGHT 31
#define PIN_I2C_SDA 45
#define PIN_I2C_SCL 46
#define PIN_TOUCH_RST 40
#define PIN_TOUCH_INT 42
#define BACKLIGHT_PWM_HZ 30000
#define MIPI_PHY_LDO_CHAN 3
#define MIPI_PHY_LDO_MV 2500

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
    const ledc_channel_config_t c = {
        .gpio_num = PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    return ledc_channel_config(&c);
}

esp_err_t board_set_backlight(uint32_t percent)
{
    if (percent > 100) percent = 100;
    // 与官方例程一致：0..100 映射到 200..2000（11 位分辨率），0 为关闭
    uint32_t duty = percent ? percent * 18 + 200 : 0;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *out)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "i2c bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch io");

    const esp_lcd_touch_config_t tc = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tc, out);
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    // MIPI DSI PHY 供电
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {.chan_id = MIPI_PHY_LDO_CHAN, .voltage_mv = MIPI_PHY_LDO_MV};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "ldo");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    board_set_backlight(0);

    esp_lcd_dsi_bus_handle_t dsi;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 900,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t dbi;
    const esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &dbi), TAG, "dbi");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 51,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_back_porch = 160,
            .hsync_pulse_width = 70,
            .hsync_front_porch = 160,
            .vsync_back_porch = 23,
            .vsync_pulse_width = 10,
            .vsync_front_porch = 12,
        },
        .flags.use_dma2d = true,
    };
    ek79007_vendor_config_t vendor = {
        .mipi_config = {.dsi_bus = dsi, .dpi_config = &dpi_cfg},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(dbi, &panel_cfg, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed, touch disabled");
        tp = NULL;
    }

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 16384,
        .task_affinity = 1,
        .task_max_sleep_ms = 50,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = dbi,
        .panel_handle = panel,
        .control_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = LCD_SWAP_BYTES,
            .full_refresh = false,
            .direct_mode = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {.flags.avoid_tearing = true};
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) return ESP_FAIL;

    if (tp) {
        const lvgl_port_touch_cfg_t tcfg = {.disp = disp, .handle = tp};
        if (!lvgl_port_add_touch(&tcfg)) ESP_LOGE(TAG, "add touch failed");
    }
    if (out_disp) *out_disp = disp;
    return ESP_OK;
}
