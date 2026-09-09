idf_component_register(
    SRCS "../../newo-rx/main/newo_rx_main.c"
         "../../newo-rx/main/ncsi_protocol.c"
         "../../newo-rx/main/probe_protocol.c"
    INCLUDE_DIRS "../../newo-rx/main"
    REQUIRES esp_event esp_netif esp_timer esp_wifi lwip nvs_flash
)
