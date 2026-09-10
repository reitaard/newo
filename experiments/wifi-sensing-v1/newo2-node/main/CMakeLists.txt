idf_component_register(
    SRCS "../../newo-rx/main/newo_rx_main.c"
         "../../newo-rx/main/ncsi_protocol.c"
         "../../newo-rx/main/probe_protocol.c"
         "../../newo-rx/main/track_protocol.c"
         "../../newo-rx/main/track_control_state.c"
         "../../newo-rx/main/sync_protocol.c"
         "../../newo-rx/main/sync_estimator.c"
         "../../newo-rx/main/rate_gate.c"
         "../../newo-rx/main/collector_discovery.c"
    INCLUDE_DIRS "../../newo-rx/main"
    REQUIRES esp_event esp_netif esp_timer esp_wifi lwip nvs_flash
)
