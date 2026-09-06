/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "diskio_impl.h"
#include "ffconf.h"
#include "ff.h"
#include "esp_log.h"
#include "diskio_usb.h"
#include "msc_scsi_bot.h"
#include "msc_common.h"
#include "usb/msc_host.h"
#include "usb/usb_types_stack.h"
#include <inttypes.h>
#include <stdint.h>

static usb_disk_t *s_disks[FF_VOLUMES] = { NULL };
static const char *TAG = "diskio_usb";

static msc_device_t *get_device(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES || s_disks[pdrv] == NULL) return NULL;
    return __containerof(s_disks[pdrv], msc_device_t, disk);
}

static DSTATUS usb_disk_initialize(BYTE pdrv)
{
    msc_device_t *dev = get_device(pdrv);
    if (dev == NULL || dev->gone || !dev->media_ready) return STA_NOINIT | STA_NODISK;
    return 0;
}

static DSTATUS usb_disk_status(BYTE pdrv)
{
    msc_device_t *dev = get_device(pdrv);
    if (dev == NULL || dev->gone || !dev->media_ready) return STA_NOINIT | STA_NODISK;
    return 0;
}

static DRESULT usb_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (buff == NULL || count == 0) return RES_PARERR;
    msc_device_t *dev = get_device(pdrv);
    if (dev == NULL || dev->gone || !dev->media_ready) return RES_NOTRDY;

    const size_t sector_size = dev->disk.block_size;
    if (sector_size == 0) return RES_NOTRDY;
    const esp_err_t err = scsi_cmd_read10(dev, buff, sector, count, sector_size);
    if (err == ESP_ERR_MSC_MOUNT_FAILED || dev->gone || !dev->media_ready) {
        dev->media_ready = false;
        return RES_NOTRDY;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read failed (%s)", esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT usb_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (buff == NULL || count == 0) return RES_PARERR;
    msc_device_t *dev = get_device(pdrv);
    if (dev == NULL || dev->gone || !dev->media_ready) return RES_NOTRDY;

    const size_t sector_size = dev->disk.block_size;
    if (sector_size == 0) return RES_NOTRDY;
    const esp_err_t err = scsi_cmd_write10(dev, buff, sector, count, sector_size);
    if (err == ESP_ERR_MSC_MOUNT_FAILED || dev->gone || !dev->media_ready) {
        dev->media_ready = false;
        return RES_NOTRDY;
    }
    if (err != ESP_OK) {
        // Do not retry an uncertain write: the target may have committed it even
        // if the host missed the CSW. Let FatFS surface the I/O error instead.
        ESP_LOGE(TAG, "write failed (%s)", esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT usb_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    msc_device_t *dev = get_device(pdrv);
    if (dev == NULL || dev->gone || !dev->media_ready) return RES_NOTRDY;
    usb_disk_t *disk = &dev->disk;

    switch (cmd) {
    case CTRL_SYNC:
        // FatFS has already issued all sector writes before CTRL_SYNC. The
        // vendored BOT layer intentionally does not invent a cache-flush opcode
        // for devices that may not support it.
        return RES_OK;
    case GET_SECTOR_COUNT:
        if (buff == NULL || disk->block_count == 0) return RES_PARERR;
        *((DWORD *)buff) = disk->block_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buff == NULL || disk->block_size < FF_MIN_SS ||
            disk->block_size > FF_MAX_SS || disk->block_size > UINT16_MAX ||
            (disk->block_size & (disk->block_size - 1)) != 0) {
            ESP_LOGE(TAG, "Unsupported block_size %"PRIu32, (uint32_t)disk->block_size);
            return RES_PARERR;
        }
        *((WORD *)buff) = (WORD)disk->block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        return RES_PARERR;
    default:
        return RES_PARERR;
    }
}

void ff_diskio_register_msc(BYTE pdrv, usb_disk_t *disk)
{
    if (pdrv >= FF_VOLUMES || disk == NULL) return;
    static const ff_diskio_impl_t usb_disk_impl = {
        .init = &usb_disk_initialize,
        .status = &usb_disk_status,
        .read = &usb_disk_read,
        .write = &usb_disk_write,
        .ioctl = &usb_disk_ioctl,
    };
    s_disks[pdrv] = disk;
    ff_diskio_register(pdrv, &usb_disk_impl);
}

void ff_diskio_unregister_msc(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES) return;
    // Clear our pointer first. Any late/racing FatFS callback now returns
    // RES_NOTRDY instead of dereferencing a device that VFS is tearing down.
    s_disks[pdrv] = NULL;
    ff_diskio_unregister(pdrv);
}

BYTE ff_diskio_get_pdrv_disk(const usb_disk_t *disk)
{
    if (disk == NULL) return 0xff;
    for (int i = 0; i < FF_VOLUMES; ++i) {
        if (disk == s_disks[i]) return (BYTE)i;
    }
    return 0xff;
}
