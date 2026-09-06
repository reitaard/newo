/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "msc_common.h"
#include "usb/msc_host_vfs.h"
#include "diskio_impl.h"
#include "diskio_usb.h"
#include "ffconf.h"
#include "ff.h"
#include "esp_idf_version.h"

#define DRIVE_STR_LEN 3

typedef struct msc_host_vfs {
    char drive[DRIVE_STR_LEN];
    char *base_path;
    uint8_t pdrv;
} msc_host_vfs_t;

static const char *TAG = "MSC VFS";

static esp_err_t msc_format_storage(size_t block_size, size_t allocation_size, const char *drv)
{
    const size_t workbuf_size = 4096;
    void *workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL) return ESP_ERR_NO_MEM;
    const size_t cluster_size = MIN(MAX(allocation_size, block_size), 128 * block_size);
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    FRESULT err = f_mkfs(drv, FM_ANY | FM_SFD, cluster_size, workbuf, workbuf_size);
#else
    const MKFS_PARM opt = {(BYTE)(FM_ANY | FM_SFD), 0, 0, 0, cluster_size};
    FRESULT err = f_mkfs(drv, &opt, workbuf, workbuf_size);
#endif
    free(workbuf);
    return err == FR_OK ? ESP_OK : ESP_ERR_MSC_FORMAT_FAILED;
}

esp_err_t msc_host_vfs_format(msc_host_device_handle_t device,
                              const esp_vfs_fat_mount_config_t *mount_config,
                              const msc_host_vfs_handle_t vfs_handle)
{
    if (device == NULL || mount_config == NULL || vfs_handle == NULL) return ESP_ERR_INVALID_ARG;
    if (!msc_host_media_ready(device)) return ESP_ERR_MSC_MOUNT_FAILED;
    const size_t block_size = ((msc_device_t *)device)->disk.block_size;
    return msc_format_storage(block_size, mount_config->allocation_unit_size, vfs_handle->drive);
}

static void dealloc_msc_vfs(msc_host_vfs_t *vfs)
{
    if (vfs == NULL) return;
    free(vfs->base_path);
    free(vfs);
}

esp_err_t msc_host_vfs_register(msc_host_device_handle_t device,
                                const char *base_path,
                                const esp_vfs_fat_mount_config_t *mount_config,
                                msc_host_vfs_handle_t *vfs_handle)
{
    if (device == NULL || base_path == NULL || mount_config == NULL || vfs_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *vfs_handle = NULL;
    if (!msc_host_media_ready(device)) return ESP_ERR_MSC_MOUNT_FAILED;

    FATFS *fs = NULL;
    BYTE pdrv = 0xff;
    bool diskio_registered = false;
    bool vfs_registered = false;
    esp_err_t ret = ESP_ERR_MSC_MOUNT_FAILED;
    msc_device_t *dev = (msc_device_t *)device;
    const size_t block_size = dev->disk.block_size;

    msc_host_vfs_t *vfs = calloc(1, sizeof(*vfs));
    if (vfs == NULL) return ESP_ERR_NO_MEM;

    ret = ff_diskio_get_drive(&pdrv);
    if (ret != ESP_OK) goto fail;
    ff_diskio_register_msc(pdrv, &dev->disk);
    diskio_registered = true;

    char drive[DRIVE_STR_LEN] = {(char)('0' + pdrv), ':', 0};
    strncpy(vfs->drive, drive, DRIVE_STR_LEN);
    vfs->base_path = strdup(base_path);
    if (vfs->base_path == NULL) { ret = ESP_ERR_NO_MEM; goto fail; }
    vfs->pdrv = pdrv;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    esp_vfs_fat_conf_t conf = {
        .base_path = base_path,
        .fat_drive = drive,
        .max_files = mount_config->max_files,
    };
    ret = esp_vfs_fat_register_cfg(&conf, &fs);
#else
    ret = esp_vfs_fat_register(base_path, drive, mount_config->max_files, &fs);
#endif
    if (ret != ESP_OK) goto fail;
    vfs_registered = true;

    FRESULT fresult = f_mount(fs, drive, 1);
    if (fresult != FR_OK) {
        if (mount_config->format_if_mount_failed &&
            (fresult == FR_NO_FILESYSTEM || fresult == FR_INT_ERR)) {
            ret = msc_format_storage(block_size, mount_config->allocation_unit_size, drive);
            if (ret != ESP_OK) goto fail;
            if (f_mount(fs, drive, 0) != FR_OK) { ret = ESP_ERR_MSC_MOUNT_FAILED; goto fail; }
        } else {
            ret = ESP_ERR_MSC_MOUNT_FAILED;
            goto fail;
        }
    }

    *vfs_handle = vfs;
    return ESP_OK;

fail:
    if (fs != NULL) f_mount(NULL, drive, 0);
    if (diskio_registered) ff_diskio_unregister_msc(pdrv);
    if (vfs_registered) esp_vfs_fat_unregister_path(base_path);
    dealloc_msc_vfs(vfs);
    return ret;
}

esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t vfs_handle)
{
    if (vfs_handle == NULL) return ESP_ERR_INVALID_ARG;
    msc_host_vfs_t *vfs = (msc_host_vfs_t *)vfs_handle;

    // First detach the FatFS volume while its diskio callbacks are still
    // registered; then clear Newo's disk pointer before releasing the VFS path.
    // If the USB device is already gone, diskio status is NOT READY and FatFS
    // unmount still remains a bounded local operation.
    f_mount(NULL, vfs->drive, 0);
    ff_diskio_unregister_msc(vfs->pdrv);
    esp_err_t ret = esp_vfs_fat_unregister_path(vfs->base_path);
    dealloc_msc_vfs(vfs);
    return ret;
}
