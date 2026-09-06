#pragma once
#define USB_HOST_UAC_VER_MAJOR 1
#define USB_HOST_UAC_VER_MINOR 5
#define USB_HOST_UAC_VER_PATCH 0
// Arduino does not execute this component's Kconfig/CMake. Keep these bounded.
#define CONFIG_UAC_DEV_ADDR_LIST_MAX 6
#define CONFIG_UAC_FREQ_NUM_MAX 16
#define CONFIG_UAC_NUM_ISOC_URBS 3
#define CONFIG_UAC_NUM_PACKETS_PER_URB 3
#define CONFIG_UAC_RINGBUF_SAFE_DELETE_WAITING_MS 50
