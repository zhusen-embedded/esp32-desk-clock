idf_component_register(SRCS "main.c" "display.c" "ui.c" "net_time.c" "ble_prov.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd bt driver nvs_flash esp_http_client json
                                esp_timer lwip lvgl)
