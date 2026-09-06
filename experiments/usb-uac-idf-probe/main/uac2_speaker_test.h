#pragma once

#include "esp_err.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t newo_uac2_speaker_test(usb_host_client_handle_t client,
                                 usb_device_handle_t dev,
                                 const usb_device_desc_t *dev_desc,
                                 const usb_config_desc_t *cfg);

#ifdef __cplusplus
}
#endif
