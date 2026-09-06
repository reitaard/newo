/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>
#include "esp_err.h"
#include "esp_check.h"
#include "diskio_usb.h"
#include "usb/usb_host.h"
#include "usb/usb_types_stack.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum {
    MSC_EP_OUT,
    MSC_EP_IN
} msc_endpoint_t;

typedef struct {
    uint16_t bulk_in_mps;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint8_t iface_num;
} msc_config_t;

typedef struct msc_host_device {
    STAILQ_ENTRY(msc_host_device) tailq_entry;
    SemaphoreHandle_t transfer_done;
    // The driver owns one reusable usb_transfer_t per MSC device. Serialize the
    // complete BOT command (CBW -> data -> CSW) so concurrent VFS/script users
    // can never interleave packets on that transfer object.
    SemaphoreHandle_t io_lock;
    usb_device_handle_t handle;
    usb_transfer_t *xfer;
    msc_config_t config;
    usb_disk_t disk;

    // USB transport and media are separate lifetimes. A reader/controller may
    // remain enumerated with no medium. DEV_GONE flips `gone` before VFS cleanup
    // so new I/O fails closed instead of touching an invalid USB handle.
    volatile bool gone;
    volatile bool media_ready;
} msc_device_t;

esp_err_t msc_bulk_transfer(msc_device_t *device_handle, uint8_t *data, size_t size, msc_endpoint_t ep);
esp_err_t msc_control_transfer(msc_device_t *device_handle, size_t len);
esp_err_t clear_feature(msc_device_t *device, uint8_t endpoint);

#define MSC_GOTO_ON_ERROR(exp) ESP_GOTO_ON_ERROR(exp, fail, TAG, "")
#define MSC_GOTO_ON_FALSE(exp, err) ESP_GOTO_ON_FALSE( (exp), err, fail, TAG, "" )
#define MSC_RETURN_ON_ERROR(exp) ESP_RETURN_ON_ERROR((exp), TAG, "")
#define MSC_RETURN_ON_FALSE(exp, err) ESP_RETURN_ON_FALSE( (exp), (err), TAG, "")
#define MSC_RETURN_ON_INVALID_ARG(exp) ESP_RETURN_ON_FALSE((exp) != NULL, ESP_ERR_INVALID_ARG, TAG, "")

#ifdef __cplusplus
}
#endif
