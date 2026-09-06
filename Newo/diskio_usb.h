/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t block_size;
    uint32_t block_count;
} usb_disk_t;

void ff_diskio_register_msc(uint8_t pdrv, usb_disk_t *disk);

/** Clear Newo's MSC disk pointer before unregistering the FatFS diskio slot. */
void ff_diskio_unregister_msc(uint8_t pdrv);

uint8_t ff_diskio_get_pdrv_disk(const usb_disk_t *disk);

#ifdef __cplusplus
}
#endif
