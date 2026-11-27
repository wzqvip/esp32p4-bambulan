file(GLOB UI_SRCS "ui/*.c" "ui/fonts/*.c")
idf_component_register(
    SRCS "main.c" "board.c" "net.c"
         "bambu/bambu_parse.c" "bambu/bambu_cmd.c" "bambu/printer.c" "bambu/camera.c" "bambu/cover.c" "bambu/gbk_table.c" "bambu/discover.c"
         ${UI_SRCS}
    INCLUDE_DIRS "." "bambu" "ui"
    EMBED_TXTFILES "certs/bambu_ca.pem"
    REQUIRES esp_lcd esp_driver_ledc esp_driver_gpio esp_driver_i2c esp_wifi esp_netif
             esp_event nvs_flash mqtt json esp_timer esp-tls esp_driver_jpeg esp_driver_ppa
             lvgl esp_lvgl_port esp_lcd_ek79007 esp_lcd_touch_gt911 esp_wifi_remote esp_hosted
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
