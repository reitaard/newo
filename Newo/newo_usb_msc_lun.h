#pragma once

#include <stdint.h>
#include <esp_err.h>
#include <usb/msc_host.h>

#ifdef __cplusplus
extern "C" {
#endif

// BOT GET_MAX_LUN. Per the USB MSC BOT spec, devices that do not implement
// GET_MAX_LUN may stall the request; Newo treats that as max_lun=0.
esp_err_t newo_msc_get_max_lun(msc_host_device_handle_t device, uint8_t *max_lun);

// Select a logical unit for subsequent SCSI CBWs. Selection is only allowed
// while media_ready is false, so a mounted filesystem can never silently jump
// between card-reader slots.
esp_err_t newo_msc_select_lun(msc_host_device_handle_t device, uint8_t lun);

// Read the current BOT LUN state for diagnostics.
esp_err_t newo_msc_get_lun_state(msc_host_device_handle_t device,
                                 uint8_t *active_lun,
                                 uint8_t *max_lun);

#ifdef __cplusplus
}
#endif
