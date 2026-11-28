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
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "freertos/semphr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_vsync;
static ppa_client_handle_t s_slide_ppa;

// DMA2D 把 LVGL 画好的区域拷进帧缓冲后：通知 LVGL（和 esp_lvgl_port 自己的回调做的事一样）
static IRAM_ATTR bool on_trans_done(esp_lcd_panel_handle_t p, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

// 屏幕扫完一帧（vsync）：给切页滑动用来对齐帧
static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t p, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    if (s_vsync) xSemaphoreGiveFromISR(s_vsync, &hp);
    return hp == pdTRUE;
}

static lv_display_t *s_disp;

// 挂不挂 vsync 回调（IDF 只在挂了 on_refresh_done 时才打开 VSYNC 中断）
static void set_vsync_cb(bool on)
{
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_color_trans_done = on_trans_done,
                                                     .on_refresh_done = on ? on_vsync : NULL};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, s_disp));
}

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

    const esp_lcd_touch_config_t tc = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
    };
    // GT911 复位时按 INT 电平选地址（0x5D 或 0x14），上电时序不稳时偶尔会落到另一个地址，
    // 所以两个地址轮流试几次
    static const uint8_t addrs[] = {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS};   // 这块板实测在 0x14
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
        for (size_t a = 0; a < sizeof(addrs) && err != ESP_OK; a++) {
            esp_lcd_panel_io_handle_t io;
            esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
            io_cfg.dev_addr = addrs[a];
            io_cfg.scl_speed_hz = 400000;
            ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch io");
            err = esp_lcd_touch_new_i2c_gt911(io, &tc, out);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "GT911 at 0x%02X (attempt %d)", addrs[a], attempt + 1);
            } else {
                esp_lcd_panel_io_del(io);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
    return err;
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
        .num_fbs = 1,   // 见下面“刷屏方式”说明，不要改成 2
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
    s_panel = panel;

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
            .sw_rotate = true,   // 不旋转；设为 true 避免 lvgl_port 调用面板不支持的 swap_xy
            .swap_bytes = LCD_SWAP_BYTES,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    // ===== 刷屏方式（实测定下来的，不要改）=====
    // 和 ELECROW 官方例程一致：局部渲染（PARTIAL）+ 1 块 DPI 帧缓冲 + 不开 avoid_tearing + 背光 100%。
    // LVGL 只画脏区域到 PSRAM 里的绘制缓冲，再由 DMA2D 拷进唯一的帧缓冲。
    // 试过的反例：direct_mode + 2 块帧缓冲 + avoid_tearing（vsync 换缓冲）会让整块屏亮度肉眼可见地闪
    // （录像测得约 ±3%，背光开到 100% 也一样）。另外界面上不要加一直在跑的动画（呼吸灯、循环动效等），
    // 持续刷新同样会把闪烁带回来。详见 README “刷屏方式”。
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {.flags.avoid_tearing = false};
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) return ESP_FAIL;
    // 自己接管 DPI 回调（register 会覆盖 esp_lvgl_port 挂的，所以 flush_ready 也自己做）。
    // vsync 中断只在切页滑动的那 250ms 里打开：常开时实测静止画面又出现 1.5~3% 的亮度抖动。
    s_vsync = xSemaphoreCreateBinary();
    s_disp = disp;
    set_vsync_cb(false);

    if (tp) {
        const lvgl_port_touch_cfg_t tcfg = {.disp = disp, .handle = tp};
        if (!lvgl_port_add_touch(&tcfg)) ESP_LOGE(TAG, "add touch failed");
    }
    if (out_disp) *out_disp = disp;
    return ESP_OK;
}

bool board_copy_framebuffer(void *dst, size_t size)
{
    void *fb = NULL;
    size_t n = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
    if (!s_panel || size < n || esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) return false;
    // 局部渲染模式下最后一块区域可能还在由 DMA2D 往帧缓冲里拷，等一下再读
    vTaskDelay(pdMS_TO_TICKS(6));
    // 帧缓冲只被 DMA 写过，CPU 缓存里可能是旧数据：先失效再读
    esp_cache_msync(fb, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(dst, fb, n);
    return true;
}


static float ease_out_cubic(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// 一块 SRM 拷贝：src 的 [sx, sx+w) 列、[y, y+h) 行 → 帧缓冲的 dx 列
static bool blit(void *fb, size_t fb_size, const void *src, int sx, int dx, int w, int y, int h)
{
    if (w <= 0) return true;
    ppa_srm_oper_config_t op = {
        .in = {.buffer = src, .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES, .block_w = w, .block_h = h,
               .block_offset_x = sx, .block_offset_y = y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = fb, .buffer_size = fb_size, .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
                .block_offset_x = dx, .block_offset_y = y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_slide_ppa, &op) == ESP_OK;
}

int board_slide(const void *old_img, const void *new_img, int dir, uint32_t ms)
{
    const int W = BOARD_LCD_H_RES, H = BOARD_LCD_V_RES, STRIP = 60;
    void *fb = NULL;
    size_t fb_size = (size_t)W * H * 2;
    if (!s_panel || !s_vsync || esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) return -1;
    if (!s_slide_ppa) {
        ppa_client_config_t pcfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
        if (ppa_register_client(&pcfg, &s_slide_ppa) != ESP_OK) return -1;
    }
    // CPU 写过的两张快照先整体写回 PSRAM（PPA 驱动每块也会同步，这里一次做完后它那边基本是空操作）
    esp_cache_msync((void *)old_img, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync((void *)new_img, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    // 等 LVGL 可能还没送完的最后一块（DMA2D）
    vTaskDelay(pdMS_TO_TICKS(6));

    int64_t t0 = esp_timer_get_time(), dur = (int64_t)ms * 1000;
    int frames = 0, missed = 0;
    int64_t copy_max = 0, copy_sum = 0;
    set_vsync_cb(true);
    xSemaphoreTake(s_vsync, 0);
    for (;;) {
        // 每帧等屏幕扫完上一帧再开始拷，从上往下按条带拷：PPA 比扫描快，始终跑在扫描线前面，不撕裂
        if (xSemaphoreTake(s_vsync, pdMS_TO_TICKS(50)) != pdTRUE) missed++;
        int64_t c0 = esp_timer_get_time();
        int64_t el = esp_timer_get_time() - t0;
        float t = el >= dur ? 1.0f : (float)el / dur;
        int off = (int)(W * ease_out_cubic(t) + 0.5f);
        if (off > W) off = W;
        for (int y = 0; y < H; y += STRIP) {
            int h = H - y < STRIP ? H - y : STRIP;
            bool ok;
            if (dir > 0) {   // 新页从右边进来
                ok = blit(fb, fb_size, old_img, off, 0, W - off, y, h) &&
                     blit(fb, fb_size, new_img, 0, W - off, off, y, h);
            } else {         // 新页从左边进来
                ok = blit(fb, fb_size, new_img, W - off, 0, off, y, h) &&
                     blit(fb, fb_size, old_img, 0, off, W - off, y, h);
            }
            if (!ok) {
                set_vsync_cb(false);
                return -1;
            }
        }
        int64_t c = esp_timer_get_time() - c0;
        copy_sum += c;
        if (c > copy_max) copy_max = c;
        frames++;
        if (t >= 1.0f) break;
    }
    set_vsync_cb(false);
    int64_t total = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "slide: %d frames in %d ms (%.1f fps), PPA copy avg %.1f max %.1f ms, vsync timeouts %d", frames,
             (int)(total / 1000), total > 0 ? frames * 1e6 / total : 0.0, copy_sum / 1000.0 / frames, copy_max / 1000.0,
             missed);
    return frames;
}
