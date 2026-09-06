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

// Keep each BOT data phase modest on ESP32-S3. FatFS may request many sectors
// at once; splitting them avoids making a single large USB transfer the failure
// domain. Reads are safe to retry once after BOT reset recovery. Writes are
// deliberately never retried because a device may have committed an interrupted
// WRITE10 even if the host did not receive the final CSW.
#define MSC_IO_MAX_BYTES 4096U

static msc_device_t *get_device(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES || s_disks[pdrv] == NULL) return NULL;
    return __containerof(s_disks[pdrv], msc_device_t, disk);
}

static UINT max_sectors_per_bot(const msc_device_t *dev)
{
    if (dev == NULL || dev->disk.block_size == 0) return 1;
    UINT sectors = (UINT)(MSC_IO_MAX_BYTES / dev->disk.block_size);
    return sectors == 0 ? 1 : sectors;
}

static esp_err_t read_chunk_with_recovery(msc_device_t *dev, BYTE *buff,
                                          DWORD sector, UINT count)
{
    esp_err_t err = scsi_cmd_read10(dev, buff, sector, count, dev->disk.block_size);
    if (err == ESP_OK) return ESP_OK;

    if (dev->gone || err == ESP_ERR_MSC_MOUNT_FAILED || !dev->media_ready) {
        dev->media_ready = false;
        return err;
    }

    // A read is idempotent, so a single BOT reset + retry is safe. This also
    // distinguishes a transient full-speed USB error from an actual removal.
    ESP_LOGW(TAG, "read transport error (%s), recovering sector=%"PRIu32" count=%u",
             esp_err_to_name(err), (uint32_t)sector, (unsigned)count);
    const esp_err_t recovery = msc_host_reset_recovery(dev);
    if (recovery != ESP_OK || dev->gone) {
        dev->media_ready = false;
        ESP_LOGE(TAG, "read recovery failed (%s)", esp_err_to_name(recovery));
        return recovery != ESP_OK ? recovery : err;
    }

    err = scsi_cmd_read10(dev, buff, sector, count, dev->disk.block_size);
    if (err != ESP_OK || dev->gone) {
        dev->media_ready = false;
        ESP_LOGE(TAG, "read retry failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "read recovered sector=%"PRIu32" count=%u",
             (uint32_t)sector, (unsigned)count);
    return ESP_OK;
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

    const UINT max_sectors = max_sectors_per_bot(dev);
    UINT remaining = count;
    DWORD current_sector = sector;
    BYTE *current = buff;

    while (remaining > 0) {
        const UINT chunk = remaining > max_sectors ? max_sectors : remaining;
        const esp_err_t err = read_chunk_with_recovery(dev, current, current_sector, chunk);
        if (err != ESP_OK) {
            if (err == ESP_ERR_MSC_MOUNT_FAILED || dev->gone || !dev->media_ready) {
                dev->media_ready = false;
                return RES_NOTRDY;
            }
            dev->media_ready = false;
            ESP_LOGE(TAG, "read failed (%s); media invalidated", esp_err_to_name(err));
            return RES_ERROR;
        }
        current += (size_t)chunk * sector_size;
        current_sector += chunk;
        remaining -= chunk;
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

    const UINT max_sectors = max_sectors_per_bot(dev);
    UINT remaining = count;
    DWORD current_sector = sector;
    const BYTE *current = buff;

    while (remaining > 0) {
        const UINT chunk = remaining > max_sectors ? max_sectors : remaining;
        const esp_err_t err = scsi_cmd_write10(dev, current, current_sector,
                                               chunk, sector_size);
        if (err != ESP_OK) {
            // Never retry an uncertain write. Fail closed and force a clean
            // remount/probe before any subsequent filesystem operation.
            dev->media_ready = false;
            if (err == ESP_ERR_MSC_MOUNT_FAILED || dev->gone) return RES_NOTRDY;
            ESP_LOGE(TAG, "write failed (%s); media invalidated", esp_err_to_name(err));
            return RES_ERROR;
        }
        current += (size_t)chunk * sector_size;
        current_sector += chunk;
        remaining -= chunk;
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
