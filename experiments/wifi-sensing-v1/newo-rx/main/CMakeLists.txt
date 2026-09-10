idf_component_register(
    SRCS "newo_rx_main.c" "ncsi_protocol.c" "probe_protocol.c" "track_protocol.c" "track_control_state.c" "sync_protocol.c" "sync_estimator.c" "rate_gate.c" "collector_discovery.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_timer esp_wifi lwip nvs_flash
)
