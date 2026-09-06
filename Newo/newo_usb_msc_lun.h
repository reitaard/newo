#pragma once

#include <stdint.h>
#include <esp_err.h>
#include <usb/msc_host.h>

#ifdef __cplusplus
extern "C" {
#endif

// BOT GET_MAX_LUN diagnostic. Per the USB MSC BOT spec, devices that do not
// implement GET_MAX_LUN may stall the request; Newo treats that as max_lun=0.
esp_err_t newo_msc_get_max_lun(msc_host_device_handle_t device, uint8_t *max_lun);

#ifdef __cplusplus
}
#endif
