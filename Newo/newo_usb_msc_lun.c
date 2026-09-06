/*
 * Newo-specific USB MSC logical-unit helpers.
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

    esp_err_t result = ESP_OK;
    if (dev->gone) {
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)dev->xfer->data_buffer;
    USB_MASS_REQ_INIT_GET_MAX_LUN(setup, dev->config.iface_num);

    const esp_err_t ret = msc_control_transfer(dev, USB_SETUP_PACKET_SIZE + 1);
    if (ret == ESP_OK) {
        // BOT bCBWLUN is four bits. Clamp malformed peers to 15.
        const uint8_t reported = dev->xfer->data_buffer[USB_SETUP_PACKET_SIZE];
        dev->max_lun = reported > 15 ? 15 : reported;
    } else if (dev->gone) {
        result = ESP_ERR_INVALID_STATE;
        goto done;
    } else {
        // BOT 1.0 permits a single-LUN device to STALL GET_MAX_LUN.
        dev->max_lun = 0;
    }

    if (dev->active_lun > dev->max_lun) dev->active_lun = 0;
    *max_lun = dev->max_lun;

done:
    xSemaphoreGiveRecursive(dev->io_lock);
    return result;
}

esp_err_t newo_msc_select_lun(msc_host_device_handle_t device, uint8_t lun)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || dev->io_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (lun > dev->max_lun || lun > 15) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTakeRecursive(dev->io_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (dev->gone) {
        result = ESP_ERR_INVALID_STATE;
    } else if (dev->media_ready && lun != dev->active_lun) {
        // Never let a mounted/ready filesystem silently switch backing LUNs.
        result = ESP_ERR_INVALID_STATE;
    } else if (lun != dev->active_lun) {
        dev->active_lun = lun;
        dev->media_ready = false;
        dev->disk.block_size = 0;
        dev->disk.block_count = 0;
    }

    xSemaphoreGiveRecursive(dev->io_lock);
    return result;
}

esp_err_t newo_msc_get_lun_state(msc_host_device_handle_t device,
                                 uint8_t *active_lun,
                                 uint8_t *max_lun)
{
    if (device == NULL || active_lun == NULL || max_lun == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone) return ESP_ERR_INVALID_STATE;
    *active_lun = dev->active_lun;
    *max_lun = dev->max_lun;
    return ESP_OK;
}
