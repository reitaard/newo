/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/param.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "diskio_usb.h"
#include "msc_common.h"
#include "usb/msc_host.h"
#include "msc_scsi_bot.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_helpers.h"

static portMUX_TYPE msc_lock = portMUX_INITIALIZER_UNLOCKED;
#define MSC_ENTER_CRITICAL() portENTER_CRITICAL(&msc_lock)
#define MSC_EXIT_CRITICAL() portEXIT_CRITICAL(&msc_lock)

#define DEFAULT_XFER_SIZE 64
#define WAIT_FOR_READY_TIMEOUT_MS 5000
#define SCSI_COMMAND_SET 0x06
#define BULK_ONLY_TRANSFER 0x50
#define MSC_NO_SENSE 0x00
#define MSC_NOT_READY 0x02
#define MSC_UNIT_ATTENTION 0x06
#define MSC_MEDIUM_NOT_PRESENT 0x3A
#define IO_DRAIN_TIMEOUT_MS 6500

#define USB_MASS_REQ_INIT_RESET(ctrl_req_ptr, intf_num) ({             \
    (ctrl_req_ptr)->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |      \
                                    USB_BM_REQUEST_TYPE_TYPE_CLASS |   \
                                    USB_BM_REQUEST_TYPE_RECIP_INTERFACE; \
    (ctrl_req_ptr)->bRequest = 0xFF;                                   \
    (ctrl_req_ptr)->wValue = 0;                                        \
    (ctrl_req_ptr)->wIndex = (intf_num);                               \
    (ctrl_req_ptr)->wLength = 0;                                       \
})

#define FEATURE_SELECTOR_ENDPOINT 0
#define USB_SETUP_PACKET_INIT_CLEAR_FEATURE_EP(ctrl_req_ptr, ep_num) ({ \
    (ctrl_req_ptr)->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |       \
                                    USB_BM_REQUEST_TYPE_TYPE_STANDARD | \
                                    USB_BM_REQUEST_TYPE_RECIP_ENDPOINT; \
    (ctrl_req_ptr)->bRequest = USB_B_REQUEST_CLEAR_FEATURE;             \
    (ctrl_req_ptr)->wValue = FEATURE_SELECTOR_ENDPOINT;                  \
    (ctrl_req_ptr)->wIndex = (ep_num);                                   \
    (ctrl_req_ptr)->wLength = 0;                                         \
})

static const char *TAG = "USB_MSC";

typedef struct {
    usb_host_client_handle_t client_handle;
    msc_host_event_cb_t user_cb;
    void *user_arg;
    SemaphoreHandle_t all_events_handled;
    volatile bool end_client_event_handling;
    bool event_handling_started;
    STAILQ_HEAD(devices, msc_host_device) devices_tailq;
} msc_driver_t;

static msc_driver_t *s_msc_driver;

static const usb_standard_desc_t *next_interface_desc(const usb_standard_desc_t *desc,
                                                       size_t len, size_t *offset)
{
    return usb_parse_next_descriptor_of_type(desc, len, USB_W_VALUE_DT_INTERFACE, (int *)offset);
}

static const usb_standard_desc_t *next_endpoint_desc(const usb_standard_desc_t *desc,
                                                      size_t len, size_t *offset)
{
    return usb_parse_next_descriptor_of_type(desc, len, USB_B_DESCRIPTOR_TYPE_ENDPOINT, (int *)offset);
}

static inline bool is_in_endpoint(uint8_t endpoint)
{
    return (endpoint & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
}

static const usb_intf_desc_t *find_msc_interface(const usb_config_desc_t *config_desc, size_t *offset)
{
    if (config_desc == NULL || offset == NULL) return NULL;
    const size_t total_length = config_desc->wTotalLength;
    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)config_desc;
    next_desc = next_interface_desc(next_desc, total_length, offset);

    while (next_desc) {
        const usb_intf_desc_t *ifc = (const usb_intf_desc_t *)next_desc;
        if (ifc->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
            ifc->bInterfaceSubClass == SCSI_COMMAND_SET &&
            ifc->bInterfaceProtocol == BULK_ONLY_TRANSFER) {
            return ifc;
        }
        next_desc = next_interface_desc(next_desc, total_length, offset);
    }
    return NULL;
}

static esp_err_t extract_config_from_descriptor(const usb_config_desc_t *cfg_desc, msc_config_t *cfg)
{
    if (cfg_desc == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));

    size_t offset = 0;
    const size_t total_len = cfg_desc->wTotalLength;
    const usb_intf_desc_t *ifc = find_msc_interface(cfg_desc, &offset);
    if (ifc == NULL) return ESP_ERR_NOT_SUPPORTED;
    cfg->iface_num = ifc->bInterfaceNumber;

    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)ifc;
    for (int i = 0; i < 2; ++i) {
        next_desc = next_endpoint_desc(next_desc, total_len, &offset);
        if (next_desc == NULL) return ESP_ERR_NOT_SUPPORTED;
        const usb_ep_desc_t *ep = (const usb_ep_desc_t *)next_desc;
        if (is_in_endpoint(ep->bEndpointAddress)) {
            cfg->bulk_in_ep = ep->bEndpointAddress;
            cfg->bulk_in_mps = ep->wMaxPacketSize;
        } else {
            cfg->bulk_out_ep = ep->bEndpointAddress;
        }
    }

    if (cfg->bulk_in_ep == 0 || cfg->bulk_out_ep == 0 || cfg->bulk_in_mps == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static bool device_is_registered(msc_device_t *dev)
{
    if (s_msc_driver == NULL || dev == NULL) return false;
    bool found = false;
    MSC_ENTER_CRITICAL();
    msc_device_t *iter;
    STAILQ_FOREACH(iter, &s_msc_driver->devices_tailq, tailq_entry) {
        if (iter == dev) {
            found = true;
            break;
        }
    }
    MSC_EXIT_CRITICAL();
    return found;
}

static esp_err_t msc_deinit_device(msc_device_t *dev, bool partial)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    dev->gone = true;
    dev->media_ready = false;

    // Never free the reusable transfer while another task is inside a BOT
    // command. A removed drive may take up to the transfer timeout to unwind.
    bool io_locked = false;
    if (dev->io_lock != NULL) {
        io_locked = xSemaphoreTakeRecursive(dev->io_lock, pdMS_TO_TICKS(IO_DRAIN_TIMEOUT_MS)) == pdTRUE;
        if (!io_locked && !partial) return ESP_ERR_TIMEOUT;
    }

    if (s_msc_driver != NULL && device_is_registered(dev)) {
        MSC_ENTER_CRITICAL();
        STAILQ_REMOVE(&s_msc_driver->devices_tailq, dev, msc_host_device, tailq_entry);
        MSC_EXIT_CRITICAL();
    }

    esp_err_t first_error = ESP_OK;
    if (s_msc_driver != NULL && s_msc_driver->client_handle != NULL && dev->handle != NULL) {
        esp_err_t e = usb_host_interface_release(s_msc_driver->client_handle,
                                                 dev->handle, dev->config.iface_num);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE && first_error == ESP_OK) first_error = e;
        e = usb_host_device_close(s_msc_driver->client_handle, dev->handle);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE && first_error == ESP_OK) first_error = e;
    }
    dev->handle = NULL;

    if (dev->xfer != NULL) {
        esp_err_t e = usb_host_transfer_free(dev->xfer);
        if (e != ESP_OK && first_error == ESP_OK) first_error = e;
        dev->xfer = NULL;
    }

    if (dev->transfer_done != NULL) {
        vSemaphoreDelete(dev->transfer_done);
        dev->transfer_done = NULL;
    }

    if (dev->io_lock != NULL) {
        if (io_locked) xSemaphoreGiveRecursive(dev->io_lock);
        vSemaphoreDelete(dev->io_lock);
        dev->io_lock = NULL;
    }

    free(dev);
    return first_error;
}

static esp_err_t msc_wait_for_ready_state(msc_device_t *dev, size_t timeout_ms)
{
    if (dev == NULL || dev->gone) return ESP_ERR_INVALID_STATE;
    const uint32_t trials = MAX(1, timeout_ms / 100);

    for (uint32_t i = 0; i <= trials; ++i) {
        if (dev->gone) return ESP_ERR_INVALID_STATE;
        esp_err_t err = scsi_cmd_unit_ready(dev);
        if (err == ESP_OK) return ESP_OK;

        scsi_sense_data_t sense = {0};
        err = scsi_cmd_sense(dev, &sense);
        if (err != ESP_OK) return err;

        if (sense.key == MSC_NOT_READY && sense.code == MSC_MEDIUM_NOT_PRESENT) {
            dev->media_ready = false;
            return ESP_ERR_MSC_MOUNT_FAILED;
        }
        if (sense.key != MSC_NOT_READY && sense.key != MSC_UNIT_ATTENTION &&
            sense.key != MSC_NO_SENSE) {
            return ESP_ERR_MSC_INTERNAL;
        }
        if (i != trials) vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

static bool is_mass_storage_device(uint8_t dev_addr)
{
    if (s_msc_driver == NULL) return false;
    usb_device_handle_t device = NULL;
    const usb_config_desc_t *config_desc = NULL;
    bool result = false;

    if (usb_host_device_open(s_msc_driver->client_handle, dev_addr, &device) == ESP_OK) {
        if (usb_host_get_active_config_descriptor(device, &config_desc) == ESP_OK) {
            size_t offset = 0;
            result = find_msc_interface(config_desc, &offset) != NULL;
        }
        usb_host_device_close(s_msc_driver->client_handle, device);
    }
    return result;
}

static msc_device_t *find_msc_device(usb_device_handle_t device_handle)
{
    if (s_msc_driver == NULL) return NULL;
    msc_device_t *found = NULL;
    MSC_ENTER_CRITICAL();
    msc_device_t *iter;
    STAILQ_FOREACH(iter, &s_msc_driver->devices_tailq, tailq_entry) {
        if (iter->handle == device_handle) {
            found = iter;
            break;
        }
    }
    MSC_EXIT_CRITICAL();
    return found;
}

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event == NULL || s_msc_driver == NULL) return;

    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (is_mass_storage_device(event->new_dev.address)) {
            const msc_host_event_t out = {
                .event = MSC_DEVICE_CONNECTED,
                .device.address = event->new_dev.address,
            };
            s_msc_driver->user_cb(&out, s_msc_driver->user_arg);
        }
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE: {
        msc_device_t *dev = find_msc_device(event->dev_gone.dev_hdl);
        if (dev != NULL) {
            // Flip these before notifying VFS/user code. New sector requests now
            // fail immediately instead of touching a handle the host marked gone.
            dev->gone = true;
            dev->media_ready = false;
            const msc_host_event_t out = {
                .event = MSC_DEVICE_DISCONNECTED,
                .device.handle = dev,
            };
            s_msc_driver->user_cb(&out, s_msc_driver->user_arg);
        }
        break;
    }

#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED: {
        msc_device_t *dev = find_msc_device(event->dev_suspend_resume.dev_hdl);
        if (dev != NULL) {
            const msc_host_event_t out = {.event = MSC_DEVICE_SUSPENDED, .device.handle = dev};
            s_msc_driver->user_cb(&out, s_msc_driver->user_arg);
        }
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED: {
        msc_device_t *dev = find_msc_device(event->dev_suspend_resume.dev_hdl);
        if (dev != NULL) {
            const msc_host_event_t out = {.event = MSC_DEVICE_RESUMED, .device.handle = dev};
            s_msc_driver->user_cb(&out, s_msc_driver->user_arg);
        }
        break;
    }
#endif
    default:
        break;
    }
}

esp_err_t msc_host_handle_events(TickType_t timeout)
{
    if (s_msc_driver == NULL) return ESP_ERR_INVALID_STATE;
    s_msc_driver->event_handling_started = true;
    const esp_err_t ret = usb_host_client_handle_events(s_msc_driver->client_handle, timeout);
    if (s_msc_driver->end_client_event_handling) {
        xSemaphoreGive(s_msc_driver->all_events_handled);
        return ESP_FAIL;
    }
    return ret;
}

static void event_handler_task(void *arg)
{
    (void)arg;
    while (msc_host_handle_events(portMAX_DELAY) == ESP_OK) {}
    vTaskDelete(NULL);
}

esp_err_t msc_host_install(const msc_host_driver_config_t *config)
{
    if (config == NULL || config->callback == NULL) return ESP_ERR_INVALID_ARG;
    if (config->create_backround_task && (config->stack_size == 0 || config->task_priority == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_msc_driver != NULL) return ESP_ERR_INVALID_STATE;

    msc_driver_t *driver = calloc(1, sizeof(*driver));
    if (driver == NULL) return ESP_ERR_NO_MEM;
    driver->user_cb = config->callback;
    driver->user_arg = config->callback_arg;
    driver->all_events_handled = xSemaphoreCreateBinary();
    if (driver->all_events_handled == NULL) {
        free(driver);
        return ESP_ERR_NO_MEM;
    }

    usb_host_client_config_t client_config = {
        .async.client_event_callback = client_event_cb,
        .async.callback_arg = NULL,
        .max_num_event_msg = 10,
    };
    esp_err_t ret = usb_host_client_register(&client_config, &driver->client_handle);
    if (ret != ESP_OK) {
        vSemaphoreDelete(driver->all_events_handled);
        free(driver);
        return ret;
    }

    STAILQ_INIT(&driver->devices_tailq);
    MSC_ENTER_CRITICAL();
    if (s_msc_driver != NULL) {
        MSC_EXIT_CRITICAL();
        usb_host_client_deregister(driver->client_handle);
        vSemaphoreDelete(driver->all_events_handled);
        free(driver);
        return ESP_ERR_INVALID_STATE;
    }
    s_msc_driver = driver;
    MSC_EXIT_CRITICAL();

    if (config->create_backround_task) {
        if (xTaskCreatePinnedToCore(event_handler_task, "USB MSC", config->stack_size,
                                    NULL, config->task_priority, NULL,
                                    config->core_id) != pdPASS) {
            MSC_ENTER_CRITICAL();
            s_msc_driver = NULL;
            MSC_EXIT_CRITICAL();
            usb_host_client_deregister(driver->client_handle);
            vSemaphoreDelete(driver->all_events_handled);
            free(driver);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t msc_host_uninstall(void)
{
    if (s_msc_driver == NULL) return ESP_ERR_INVALID_STATE;

    MSC_ENTER_CRITICAL();
    if (!STAILQ_EMPTY(&s_msc_driver->devices_tailq) || s_msc_driver->end_client_event_handling) {
        MSC_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    s_msc_driver->end_client_event_handling = true;
    MSC_EXIT_CRITICAL();

    esp_err_t first_error = ESP_OK;
    if (s_msc_driver->event_handling_started) {
        esp_err_t e = usb_host_client_unblock(s_msc_driver->client_handle);
        if (e != ESP_OK && first_error == ESP_OK) first_error = e;
        if (xSemaphoreTake(s_msc_driver->all_events_handled, pdMS_TO_TICKS(2000)) != pdTRUE &&
            first_error == ESP_OK) {
            first_error = ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t e = usb_host_client_deregister(s_msc_driver->client_handle);
    if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    vSemaphoreDelete(s_msc_driver->all_events_handled);
    msc_driver_t *old = s_msc_driver;
    s_msc_driver = NULL;
    free(old);
    return first_error;
}

esp_err_t msc_host_install_device_transport(uint8_t device_address,
                                            msc_host_device_handle_t *device_out)
{
    if (device_out == NULL) return ESP_ERR_INVALID_ARG;
    *device_out = NULL;
    if (s_msc_driver == NULL || s_msc_driver->client_handle == NULL) return ESP_ERR_INVALID_STATE;

    msc_device_t *dev = calloc(1, sizeof(*dev));
    if (dev == NULL) return ESP_ERR_NO_MEM;

    MSC_ENTER_CRITICAL();
    STAILQ_INSERT_TAIL(&s_msc_driver->devices_tailq, dev, tailq_entry);
    MSC_EXIT_CRITICAL();

    esp_err_t ret = ESP_OK;
    const usb_config_desc_t *config_desc = NULL;
    dev->transfer_done = xSemaphoreCreateBinary();
    if (dev->transfer_done == NULL) { ret = ESP_ERR_NO_MEM; goto fail; }
    dev->io_lock = xSemaphoreCreateRecursiveMutex();
    if (dev->io_lock == NULL) { ret = ESP_ERR_NO_MEM; goto fail; }

    ret = usb_host_device_open(s_msc_driver->client_handle, device_address, &dev->handle);
    if (ret != ESP_OK) goto fail;
    ret = usb_host_get_active_config_descriptor(dev->handle, &config_desc);
    if (ret != ESP_OK) goto fail;
    ret = extract_config_from_descriptor(config_desc, &dev->config);
    if (ret != ESP_OK) goto fail;
    ret = usb_host_transfer_alloc(DEFAULT_XFER_SIZE, 0, &dev->xfer);
    if (ret != ESP_OK) goto fail;
    ret = usb_host_interface_claim(s_msc_driver->client_handle, dev->handle,
                                   dev->config.iface_num, 0);
    if (ret != ESP_OK) goto fail;

    dev->gone = false;
    dev->media_ready = false;
    ret = scsi_cmd_inquiry(dev);
    if (ret != ESP_OK) goto fail;

    *device_out = dev;
    return ESP_OK;

fail:
    msc_deinit_device(dev, true);
    return ret;
}

esp_err_t msc_host_probe_media(msc_host_device_handle_t device)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone) return ESP_ERR_INVALID_STATE;

    dev->media_ready = false;
    dev->disk.block_size = 0;
    dev->disk.block_count = 0;

    esp_err_t ret = msc_wait_for_ready_state(dev, WAIT_FOR_READY_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    uint32_t block_size = 0;
    uint32_t block_count = 0;
    ret = scsi_cmd_read_capacity(dev, &block_size, &block_count);
    if (ret != ESP_OK) return ret;
    if (block_size < 512 || block_size > 4096 ||
        (block_size & (block_size - 1)) != 0 || block_count == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    dev->disk.block_size = block_size;
    dev->disk.block_count = block_count;
    dev->media_ready = true;
    return ESP_OK;
}

bool msc_host_media_ready(msc_host_device_handle_t device)
{
    if (device == NULL) return false;
    const msc_device_t *dev = (const msc_device_t *)device;
    return !dev->gone && dev->media_ready;
}

esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *device)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    *device = NULL;

    msc_host_device_handle_t dev = NULL;
    esp_err_t ret = msc_host_install_device_transport(device_address, &dev);
    if (ret != ESP_OK) return ret;
    ret = msc_host_probe_media(dev);
    if (ret != ESP_OK) {
        msc_host_uninstall_device(dev);
        return ret;
    }
    *device = dev;
    return ESP_OK;
}

esp_err_t msc_host_uninstall_device(msc_host_device_handle_t device)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    return msc_deinit_device((msc_device_t *)device, false);
}

esp_err_t msc_host_read_sector(msc_host_device_handle_t device, size_t sector, void *data, size_t size)
{
    if (device == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || !dev->media_ready) return ESP_ERR_MSC_MOUNT_FAILED;
    if (size != dev->disk.block_size) return ESP_ERR_INVALID_SIZE;
    return scsi_cmd_read10(dev, data, sector, 1, dev->disk.block_size);
}

esp_err_t msc_host_write_sector(msc_host_device_handle_t device, size_t sector,
                                const void *data, size_t size)
{
    if (device == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || !dev->media_ready) return ESP_ERR_MSC_MOUNT_FAILED;
    if (size != dev->disk.block_size) return ESP_ERR_INVALID_SIZE;
    return scsi_cmd_write10(dev, data, sector, 1, dev->disk.block_size);
}

static void copy_string_desc(wchar_t *dest, const usb_str_desc_t *src)
{
    if (dest == NULL) return;
    if (src == NULL) {
        dest[0] = 0;
        return;
    }
    size_t len = MIN((src->bLength - USB_STANDARD_DESC_SIZE) / 2, MSC_STR_DESC_SIZE - 1);
    for (size_t i = 0; i < len; ++i) dest[i] = (wchar_t)src->wData[i];
    dest[len] = 0;
}

esp_err_t msc_host_get_device_info(msc_host_device_handle_t device,
                                   msc_host_device_info_t *info)
{
    if (device == NULL || info == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || dev->handle == NULL) return ESP_ERR_INVALID_STATE;

    const usb_device_desc_t *desc = NULL;
    usb_device_info_t dev_info = {0};
    esp_err_t ret = usb_host_get_device_descriptor(dev->handle, &desc);
    if (ret != ESP_OK) return ret;
    ret = usb_host_device_info(dev->handle, &dev_info);
    if (ret != ESP_OK) return ret;

    memset(info, 0, sizeof(*info));
    info->idProduct = desc->idProduct;
    info->idVendor = desc->idVendor;
    info->sector_size = dev->disk.block_size;
    info->sector_count = dev->disk.block_count;
    copy_string_desc(info->iManufacturer, dev_info.str_desc_manufacturer);
    copy_string_desc(info->iProduct, dev_info.str_desc_product);
    copy_string_desc(info->iSerialNumber, dev_info.str_desc_serial_num);
    return ESP_OK;
}

esp_err_t msc_host_print_descriptors(msc_host_device_handle_t device)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || dev->handle == NULL) return ESP_ERR_INVALID_STATE;
    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    esp_err_t ret = usb_host_get_device_descriptor(dev->handle, &device_desc);
    if (ret != ESP_OK) return ret;
    ret = usb_host_get_active_config_descriptor(dev->handle, &config_desc);
    if (ret != ESP_OK) return ret;
    usb_print_device_descriptor(device_desc);
    usb_print_config_descriptor(config_desc, NULL);
    return ESP_OK;
}

static void transfer_callback(usb_transfer_t *transfer)
{
    if (transfer == NULL || transfer->context == NULL) return;
    msc_device_t *device = (msc_device_t *)transfer->context;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED && !device->gone) {
        ESP_LOGE("Transfer failed", "Status %d", transfer->status);
    }
    if (device->transfer_done != NULL) xSemaphoreGive(device->transfer_done);
}

static usb_transfer_status_t wait_for_transfer_done(usb_transfer_t *xfer)
{
    msc_device_t *device = (msc_device_t *)xfer->context;
    const BaseType_t received = xSemaphoreTake(device->transfer_done,
                                               pdMS_TO_TICKS(xfer->timeout_ms));
    if (received == pdTRUE) return xfer->status;

    if (!device->gone && xfer->device_handle != NULL) {
        usb_host_endpoint_halt(xfer->device_handle, xfer->bEndpointAddress);
        const esp_err_t flushed = usb_host_endpoint_flush(xfer->device_handle,
                                                          xfer->bEndpointAddress);
        usb_host_endpoint_clear(xfer->device_handle, xfer->bEndpointAddress);
        if (flushed == ESP_OK) {
            // A successful flush should return the transfer through its callback.
            // Bound this wait too; never deadlock Newo's storage worker forever.
            xSemaphoreTake(device->transfer_done, pdMS_TO_TICKS(250));
        }
    }
    return USB_TRANSFER_STATUS_TIMED_OUT;
}

esp_err_t msc_bulk_transfer(msc_device_t *device, uint8_t *data, size_t size,
                            msc_endpoint_t ep)
{
    if (device == NULL || data == NULL || device->xfer == NULL) return ESP_ERR_INVALID_ARG;
    if (device->gone || device->handle == NULL) return ESP_ERR_INVALID_STATE;

    usb_transfer_t *xfer = device->xfer;
    const size_t transfer_size = (ep == MSC_EP_IN)
        ? usb_round_up_to_mps(size, device->config.bulk_in_mps) : size;

    if (xfer->data_buffer_size < transfer_size) {
        esp_err_t ret = usb_host_transfer_free(xfer);
        if (ret != ESP_OK) return ret;
        device->xfer = NULL;
        ret = usb_host_transfer_alloc(transfer_size, 0, &device->xfer);
        if (ret != ESP_OK) return ret;
        xfer = device->xfer;
    }

    if (ep == MSC_EP_IN) {
        xfer->bEndpointAddress = device->config.bulk_in_ep;
    } else {
        xfer->bEndpointAddress = device->config.bulk_out_ep;
        memcpy(xfer->data_buffer, data, size);
    }
    xfer->num_bytes = transfer_size;
    xfer->device_handle = device->handle;
    xfer->callback = transfer_callback;
    xfer->timeout_ms = 5000;
    xfer->context = device;

    esp_err_t ret = usb_host_transfer_submit(xfer);
    if (ret != ESP_OK) return ret;
    const usb_transfer_status_t status = wait_for_transfer_done(xfer);
    if (status == USB_TRANSFER_STATUS_COMPLETED) {
        if (ep == MSC_EP_IN) {
            if (xfer->actual_num_bytes > size) return ESP_ERR_INVALID_SIZE;
            memcpy(data, xfer->data_buffer, xfer->actual_num_bytes);
        }
        return ESP_OK;
    }
    if (status == USB_TRANSFER_STATUS_STALL) return ESP_ERR_MSC_STALL;
    return device->gone ? ESP_ERR_INVALID_STATE : ESP_ERR_MSC_INTERNAL;
}

esp_err_t msc_control_transfer(msc_device_t *device, size_t len)
{
    if (device == NULL || device->xfer == NULL) return ESP_ERR_INVALID_ARG;
    if (device->gone || device->handle == NULL || s_msc_driver == NULL) return ESP_ERR_INVALID_STATE;
    usb_transfer_t *xfer = device->xfer;
    xfer->device_handle = device->handle;
    xfer->bEndpointAddress = 0;
    xfer->callback = transfer_callback;
    xfer->timeout_ms = 5000;
    xfer->num_bytes = len;
    xfer->context = device;
    esp_err_t ret = usb_host_transfer_submit_control(s_msc_driver->client_handle, xfer);
    if (ret != ESP_OK) return ret;
    const usb_transfer_status_t status = wait_for_transfer_done(xfer);
    if (status == USB_TRANSFER_STATUS_COMPLETED) return ESP_OK;
    return device->gone ? ESP_ERR_INVALID_STATE : ESP_ERR_MSC_INTERNAL;
}

esp_err_t clear_feature(msc_device_t *device, uint8_t endpoint)
{
    if (device == NULL || device->gone || device->handle == NULL || device->xfer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = usb_host_endpoint_halt(device->handle, endpoint);
    if (ret != ESP_OK) return ret;
    ret = usb_host_endpoint_flush(device->handle, endpoint);
    if (ret != ESP_OK) return ret;
    ret = usb_host_endpoint_clear(device->handle, endpoint);
    if (ret != ESP_OK) return ret;

    USB_SETUP_PACKET_INIT_CLEAR_FEATURE_EP((usb_setup_packet_t *)device->xfer->data_buffer,
                                           endpoint);
    return msc_control_transfer(device, USB_SETUP_PACKET_SIZE);
}

static esp_err_t msc_mass_reset(msc_host_device_handle_t device)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone || dev->xfer == NULL) return ESP_ERR_INVALID_STATE;
    USB_MASS_REQ_INIT_RESET((usb_setup_packet_t *)dev->xfer->data_buffer,
                            dev->config.iface_num);
    return msc_control_transfer(dev, USB_SETUP_PACKET_SIZE);
}

esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device)
{
    if (device == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *dev = (msc_device_t *)device;
    if (dev->gone) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = msc_mass_reset(dev);
    if (ret != ESP_OK) return ret;
    // Clearing a non-stalled endpoint may fail; reset recovery still proceeds.
    clear_feature(dev, dev->config.bulk_in_ep);
    clear_feature(dev, dev->config.bulk_out_ep);
    return msc_wait_for_ready_state(dev, WAIT_FOR_READY_TIMEOUT_MS);
}
