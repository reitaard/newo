idf_component_register(
  SRCS
    "app_main.cpp"
    "led_off.cpp"
    "newo2_event.cpp"
    "newo2_camera.cpp"
    "newo2_console.cpp"
    "newo2_network.cpp"
    "newo2_media.cpp"
    "newo2_storage.cpp"
    "skills/motion_snapshot/motion_snapshot.cpp"
  INCLUDE_DIRS "." "skills/motion_snapshot"
  REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client mbedtls json fatfs sdmmc esp_timer
)
