/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <wchar.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_MSC_HOST_BASE        0x1700                      /*!< MSC host error code base */
#define ESP_ERR_MSC_MOUNT_FAILED    (ESP_ERR_MSC_HOST_BASE + 1)  /*!< Media not ready / mount failed */
#define ESP_ERR_MSC_FORMAT_FAILED   (ESP_ERR_MSC_HOST_BASE + 2)  /*!< Failed to format storage */
#define ESP_ERR_MSC_INTERNAL        (ESP_ERR_MSC_HOST_BASE + 3)  /*!< MSC host internal error */
#define ESP_ERR_MSC_STALL           (ESP_ERR_MSC_HOST_BASE + 4)  /*!< USB transfer stalled */

/** @brief Maximum string descriptor length returned by the MSC host driver. */
#define MSC_STR_DESC_SIZE 32

#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
#define MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
#endif

typedef struct msc_host_device *msc_host_device_handle_t;

typedef struct {
    enum {
        MSC_DEVICE_CONNECTED,
        MSC_DEVICE_DISCONNECTED,
#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
        MSC_DEVICE_SUSPENDED,
        MSC_DEVICE_RESUMED,
#endif
    } event;
    union {
        uint8_t address;
        msc_host_device_handle_t handle;
    } device;
} msc_host_event_t;

typedef void (*msc_host_event_cb_t)(const msc_host_event_t *event, void *arg);

typedef struct {
    bool create_backround_task;
    size_t task_priority;
    size_t stack_size;
    BaseType_t core_id;
    msc_host_event_cb_t callback;
    void *callback_arg;
} msc_host_driver_config_t;

typedef struct {
    uint32_t sector_count;
    uint32_t sector_size;
    uint16_t idProduct;
    uint16_t idVendor;
    wchar_t iManufacturer[MSC_STR_DESC_SIZE];
    wchar_t iProduct[MSC_STR_DESC_SIZE];
    wchar_t iSerialNumber[MSC_STR_DESC_SIZE];
} msc_host_device_info_t;

esp_err_t msc_host_install(const msc_host_driver_config_t *config);
esp_err_t msc_host_uninstall(void);

/**
 * @brief Open/claim the MSC transport without requiring storage media to be ready.
 *
 * This is Newo's recovery-safe entry point for card readers and thumb-drive
 * controllers which enumerate correctly but temporarily report SCSI NOT READY.
 * The returned handle remains valid until msc_host_uninstall_device() or a USB
 * disconnect event. Call msc_host_probe_media() before mounting FatFS.
 */
esp_err_t msc_host_install_device_transport(uint8_t device_address,
                                            msc_host_device_handle_t *device);

/**
 * @brief Probe media readiness/capacity on an already-open MSC transport.
 *
 * Returns ESP_ERR_MSC_MOUNT_FAILED for NOT READY / MEDIUM NOT PRESENT (ASC 0x3A)
 * without destroying the USB handle, so callers may retry later.
 */
esp_err_t msc_host_probe_media(msc_host_device_handle_t device);

/** @brief True only after a successful media probe and before disconnect/removal. */
bool msc_host_media_ready(msc_host_device_handle_t device);

/**
 * @brief Compatibility helper: open transport, probe media and tear the device
 * back down if media is not ready.
 */
esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *device);

esp_err_t msc_host_uninstall_device(msc_host_device_handle_t device);

esp_err_t msc_host_read_sector(msc_host_device_handle_t device, size_t sector, void *data, size_t size)
__attribute__((deprecated("use API from esp_private/msc_scsi_bot.h")));

esp_err_t msc_host_write_sector(msc_host_device_handle_t device, size_t sector, const void *data, size_t size)
__attribute__((deprecated("use API from esp_private/msc_scsi_bot.h")));

esp_err_t msc_host_handle_events(TickType_t timeout);
esp_err_t msc_host_get_device_info(msc_host_device_handle_t device, msc_host_device_info_t *info);
esp_err_t msc_host_print_descriptors(msc_host_device_handle_t device);
esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device);

#ifdef __cplusplus
}
#endif //__cplusplus
