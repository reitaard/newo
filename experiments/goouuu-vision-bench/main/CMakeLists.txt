idf_component_register(
    SRCS "main_recognition.cpp" "newo_face_detect_bridge.cpp" "led_off.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_timer
)

# ESPDet-224 is the detector we physically validated, but ESPDet intentionally
# emits no facial landmarks. Rename the wrapper class only inside this component
# so newo_face_detect_bridge.cpp can preserve ESPDet boxes and attach MNP's five
# landmarks before the existing MFN recognition code sees each result.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HumanFaceDetect=NewoFaceDetect)
