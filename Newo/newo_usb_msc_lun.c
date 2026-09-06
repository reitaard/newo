/*
 * Newo-specific USB MSC diagnostics/recovery helpers.
 *
 * GET_MAX_LUN is intentionally kept outside Espressif's vendored MSC driver so
 * we can diagnose multi-slot readers without changing the proven LUN0 data path
 * before the physical device tells us what it actually exposes.
 */

#include "newo_usb_msc_lun.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <usb/usb_types_ch9.h>

#include "msc_common.h"

#define USB_MASS_REQ_INIT_GET_MAX_LUN(ctrl_req_ptr, intf_num) ({             \
    (ctrl_req_ptr)->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |             \
                                    USB_BM_REQUEST_TYPE_TYPE_CLASS |          \
                                    USB_BM_REQUEST_TYPE_RECIP_INTERFACE;      \
    (ctrl_req_ptr)->bRequest = 0xFE;                                          \
    (ctrl_req_ptr)->wValue = 0;                                               \
    (ctrl_req_ptr)->wIndex = (intf_num);                                      \
    (ctrl_req_ptr)->wLength = 1;                                              \
})

esp_err_t newo_msc_get_max_lun(msc_host_device_handle_t device, uint8_t *max_lun)
{
    if (device == NULL || max_lun == NULL) return ESP_ERR_INVALID_ARG;
    *max_lun = 0;

    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || dev->handle == NULL || dev->xfer == NULL || dev->io_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(dev->io_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (dev->gone) {
        xSemaphoreGiveRecursive(dev->io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)dev->xfer->data_buffer;
    USB_MASS_REQ_INIT_GET_MAX_LUN(setup, dev->config.iface_num);

    const esp_err_t ret = msc_control_transfer(dev, USB_SETUP_PACKET_SIZE + 1);
    if (ret == ESP_OK) {
        // BOT allows LUN values 0..15. Clamp a malformed peer rather than ever
        // emitting an out-of-range CBW value in later diagnostics.
        const uint8_t reported = dev->xfer->data_buffer[USB_SETUP_PACKET_SIZE];
        *max_lun = reported > 15 ? 15 : reported;
    } else if (dev->gone) {
        xSemaphoreGiveRecursive(dev->io_lock);
        return ESP_ERR_INVALID_STATE;
    } else {
        // BOT 1.0 explicitly permits a single-LUN device to STALL GET_MAX_LUN.
        // The current control helper folds STALL into a transport error, so any
        // failure here falls back to the required single-LUN assumption. The
        // normal INQUIRY/TUR path remains authoritative for actual media health.
        *max_lun = 0;
    }

    xSemaphoreGiveRecursive(dev->io_lock);
    return ESP_OK;
}
