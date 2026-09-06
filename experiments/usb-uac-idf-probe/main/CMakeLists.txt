idf_component_register(
    SRCS
        "speaker_probe_main.c"
        "uac2_speaker_test.c"
    INCLUDE_DIRS "."
    REQUIRES usb
)
