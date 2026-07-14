idf_component_register(SRCS "main.c"
                         "app_lcd.c"
                         "app_mic.c"
                         "app_wifi.c"
                         "app_stt.c"
                         "app_llm.c"
                         "app_speaker.c"
                         "fonts/app_font_chinese_16.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_spi esp_lcd
                             esp_wifi esp_event esp_netif nvs_flash esp_http_server
                             esp_http_client
                             esp_websocket_client esp_lvgl_port lvgl
                             esp-tls mbedtls json)
