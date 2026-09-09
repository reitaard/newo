idf_component_register(
    SRCS "newo_rx_main.c" "ncsi_protocol.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_timer esp_wifi lwip nvs_flash
)
