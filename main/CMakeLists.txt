idf_component_register(
    SRCS "ota_manager.c" "gas_sensor.c" "main.c" "wifi_config.cpp" "NeonLED.cpp" "gsm_manager.c" "time_manager.c" "data_reporter.c" "ota_manager.c"
    INCLUDE_DIRS "." "include"
    REQUIRES "led_strip" "esp_wifi" "nvs_flash" "esp_event" "esp_netif" "esp_http_server" "driver" "lwip" "esp_timer" "esp_system" "esp_http_client" "json" "esp-tls" "esp_adc" "esp_app_format" "esp_https_ota " "app_update " "mqtt"
)