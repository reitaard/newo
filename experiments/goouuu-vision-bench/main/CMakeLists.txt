idf_component_register(
    SRCS "main.cpp" "led_off.cpp" "remote_log.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_timer
)
