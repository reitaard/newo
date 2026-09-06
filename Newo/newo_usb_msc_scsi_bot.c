/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "msc_common.h"
#include "msc_scsi_bot.h"
#include "usb/msc_host.h"

static const char *TAG = "USB_MSC_SCSI";

#define SCSI_CMD_INQUIRY 0x12
#define SCSI_CMD_MODE_SENSE 0x5A
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_CMD_READ10 0x28
#define SCSI_CMD_READ_CAPACITY 0x25
#define SCSI_CMD_REQUEST_SENSE 0x03
#define SCSI_CMD_TEST_UNIT_READY 0x00
#define SCSI_CMD_WRITE10 0x2A

#define IN_DIR CWB_FLAG_DIRECTION_IN
#define OUT_DIR 0
#define CBW_CMD_SIZE(cmd) (sizeof(cmd) - sizeof(msc_cbw_t))
#define CSW_SIGNATURE 0x53425355
#define CBW_SIZE 31
#define CWB_FLAG_DIRECTION_IN (1 << 7)
#define MSC_SENSE_NOT_READY 0x02
#define MSC_ASC_MEDIUM_NOT_PRESENT 0x3A

#define CBW_BASE_INIT(dir, cbw_len, data_len) \
    .base = {                                  \
        .signature = 0x43425355,               \
        .tag = ++cbw_tag,                      \
        .data_length = data_len,               \
        .flags = dir,                          \
        .lun = 0,                              \
        .cbw_length = cbw_len,                 \
    }

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cbw_length;
} msc_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t dataResidue;
    uint8_t status;
} msc_csw_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint32_t address;
    uint8_t reserved1;
    uint16_t length;
    uint8_t reserved2[3];
} cbw_read10_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint32_t address;
    uint8_t reserved1;
    uint16_t length;
    uint8_t reserved2[1];
} cbw_write10_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved[6];
} cbw_read_capacity_t;

typedef struct __attribute__((packed)) {
    uint32_t block_count;
    uint32_t block_size;
} cbw_read_capacity_response_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved[10];
} cbw_unit_ready_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved_0[2];
    uint8_t allocation_length;
    uint8_t reserved_1[7];
} cbw_sense_t;

typedef struct __attribute__((packed)) {
    uint8_t error_code;
    uint8_t reserved_0;
    uint8_t sense_key;
    uint32_t info;
    uint8_t sense_len;
    uint32_t reserved_1;
    uint8_t sense_code;
    uint8_t sense_code_qualifier;
    uint32_t reserved_2;
} cbw_sense_response_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t page_code;
    uint8_t reserved_0;
    uint8_t allocation_length;
    uint8_t reserved_1[7];
} cbw_inquiry_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t pc_page_code;
    uint8_t reserved_1[4];
    uint16_t parameter_list_length;
    uint8_t reserved_2[3];
} mode_sense_t;

typedef struct __attribute__((packed)) {
    uint8_t data[8];
} mode_sense_response_t;

typedef struct __attribute__((packed)) {
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved_1[2];
    uint8_t prevent;
    uint8_t reserved_2[7];
} prevent_allow_medium_removal_t;

typedef struct __attribute__((packed)) {
    uint8_t data[36];
} cbw_inquiry_response_t;

static uint32_t cbw_tag;

static esp_err_t check_csw(const msc_csw_t *csw, uint32_t tag)
{
    const bool ok = csw->signature == CSW_SIGNATURE && csw->tag == tag &&
                    csw->dataResidue == 0 && csw->status == 0;
    if (!ok) {
        ESP_LOGD(TAG, "CSW failed sig=0x%08"PRIx32" tag=0x%08"PRIx32" residue=%"PRIu32" status=%u",
                 csw->signature, csw->tag, csw->dataResidue, csw->status);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static bool medium_absent(const scsi_sense_data_t *sense)
{
    return sense && sense->key == MSC_SENSE_NOT_READY &&
           sense->code == MSC_ASC_MEDIUM_NOT_PRESENT;
}

/*
 * Serialize the complete Bulk-Only Transport transaction. Newo may eventually
 * read scripts while other tasks access /usb; the MSC implementation owns one
 * reusable usb_transfer_t, so CBW/data/CSW must never interleave.
 *
 * The mutex is recursive because BOT reset recovery can issue TEST UNIT READY
 * while recovering a stalled BOT command.
 */
esp_err_t bot_execute_command(msc_device_t *device, msc_cbw_t *cbw, void *data, size_t size)
{
    if (device == NULL || cbw == NULL || device->io_lock == NULL || device->gone) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTakeRecursive(device->io_lock, pdMS_TO_TICKS(6000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    msc_csw_t csw = {0};
    const msc_endpoint_t ep = (cbw->flags & CWB_FLAG_DIRECTION_IN) ? MSC_EP_IN : MSC_EP_OUT;

    if (device->gone) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    ret = msc_bulk_transfer(device, (uint8_t *)cbw, CBW_SIZE, MSC_EP_OUT);
    if (ret != ESP_OK) goto done;

    if (data != NULL) {
        ret = msc_bulk_transfer(device, (uint8_t *)data, size, ep);
        if (ret != ESP_OK) goto done;
    }

    ret = msc_bulk_transfer(device, (uint8_t *)&csw, sizeof(csw), MSC_EP_IN);
    if (ret == ESP_ERR_MSC_STALL && !device->gone) {
        esp_err_t clear = clear_feature(device, device->config.bulk_in_ep);
        if (clear == ESP_OK) {
            ret = msc_bulk_transfer(device, (uint8_t *)&csw, sizeof(csw), MSC_EP_IN);
        }
        if (ret != ESP_OK && !device->gone) {
            // Recovery is best-effort. The original command still fails; callers
            // decide whether a read can be retried or a write must abort.
            msc_host_reset_recovery(device);
        }
    }
    if (ret == ESP_OK) ret = check_csw(&csw, cbw->tag);

done:
    xSemaphoreGiveRecursive(device->io_lock);
    return ret;
}

esp_err_t scsi_cmd_sense(msc_host_device_handle_t dev, scsi_sense_data_t *sense)
{
    msc_device_t *device = (msc_device_t *)dev;
    if (device == NULL || device->gone) return ESP_ERR_INVALID_STATE;

    cbw_sense_response_t response = {0};
    cbw_sense_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_sense_t), sizeof(response)),
        .opcode = SCSI_CMD_REQUEST_SENSE,
        .allocation_length = sizeof(response),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, &response, sizeof(response));
    if (ret != ESP_OK) return ret;

    if (sense != NULL) {
        sense->key = response.sense_key;
        sense->code = response.sense_code;
        sense->code_q = response.sense_code_qualifier;
    } else {
        ESP_LOGE(TAG, "Sense error codes: Sense Key 0x%02"PRIx8", ASC: 0x%02"PRIx8", ASCQ: 0x%02"PRIx8,
                 response.sense_key, response.sense_code, response.sense_code_qualifier);
    }
    return ESP_OK;
}

esp_err_t scsi_cmd_unit_ready(msc_host_device_handle_t dev)
{
    msc_device_t *device = (msc_device_t *)dev;
    if (device == NULL || device->gone) return ESP_ERR_INVALID_STATE;

    cbw_unit_ready_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_unit_ready_t), 0),
        .opcode = SCSI_CMD_TEST_UNIT_READY,
    };

    // Sense ownership belongs to the caller. Consuming REQUEST SENSE here would
    // clear ASC 0x3A before msc_host_probe_media() can classify it.
    return bot_execute_command(device, &cbw.base, NULL, 0);
}

esp_err_t scsi_cmd_read10(msc_host_device_handle_t dev,
                          uint8_t *data,
                          uint32_t sector_address,
                          uint32_t num_sectors,
                          uint32_t sector_size)
{
    if (dev == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    if (num_sectors != 0 && sector_size > UINT32_MAX / num_sectors) return ESP_ERR_INVALID_SIZE;
    msc_device_t *device = (msc_device_t *)dev;
    if (device->gone || !device->media_ready) return ESP_ERR_MSC_MOUNT_FAILED;

    cbw_read10_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_read10_t), num_sectors * sector_size),
        .opcode = SCSI_CMD_READ10,
        .flags = 0,
        .address = __builtin_bswap32(sector_address),
        .length = __builtin_bswap16(num_sectors),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, data, num_sectors * sector_size);
    if (ret != ESP_OK && !device->gone) {
        scsi_sense_data_t sense = {0};
        if (scsi_cmd_sense(device, &sense) == ESP_OK && medium_absent(&sense)) {
            device->media_ready = false;
            return ESP_ERR_MSC_MOUNT_FAILED;
        }
    }
    return ret;
}

esp_err_t scsi_cmd_write10(msc_host_device_handle_t dev,
                           const uint8_t *data,
                           uint32_t sector_address,
                           uint32_t num_sectors,
                           uint32_t sector_size)
{
    if (dev == NULL || data == NULL) return ESP_ERR_INVALID_ARG;
    if (num_sectors != 0 && sector_size > UINT32_MAX / num_sectors) return ESP_ERR_INVALID_SIZE;
    msc_device_t *device = (msc_device_t *)dev;
    if (device->gone || !device->media_ready) return ESP_ERR_MSC_MOUNT_FAILED;

    cbw_write10_t cbw = {
        CBW_BASE_INIT(OUT_DIR, CBW_CMD_SIZE(cbw_write10_t), num_sectors * sector_size),
        .opcode = SCSI_CMD_WRITE10,
        .address = __builtin_bswap32(sector_address),
        .length = __builtin_bswap16(num_sectors),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, (void *)data, num_sectors * sector_size);
    if (ret != ESP_OK && !device->gone) {
        scsi_sense_data_t sense = {0};
        if (scsi_cmd_sense(device, &sense) == ESP_OK && medium_absent(&sense)) {
            device->media_ready = false;
            return ESP_ERR_MSC_MOUNT_FAILED;
        }
    }
    // Never blindly retry writes after a transport error: the target may have
    // committed the data even when the host did not receive a valid CSW.
    return ret;
}

esp_err_t scsi_cmd_read_capacity(msc_host_device_handle_t dev, uint32_t *block_size, uint32_t *block_count)
{
    if (dev == NULL || block_size == NULL || block_count == NULL) return ESP_ERR_INVALID_ARG;
    msc_device_t *device = (msc_device_t *)dev;
    if (device->gone) return ESP_ERR_INVALID_STATE;

    cbw_read_capacity_response_t response = {0};
    cbw_read_capacity_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_read_capacity_t), sizeof(response)),
        .opcode = SCSI_CMD_READ_CAPACITY,
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, &response, sizeof(response));
    if (ret != ESP_OK && !device->gone) {
        scsi_sense_data_t sense = {0};
        if (scsi_cmd_sense(device, &sense) == ESP_OK && medium_absent(&sense)) {
            device->media_ready = false;
            return ESP_ERR_MSC_MOUNT_FAILED;
        }
        return ret;
    }

    *block_count = __builtin_bswap32(response.block_count);
    *block_size = __builtin_bswap32(response.block_size);
    return ESP_OK;
}

esp_err_t scsi_cmd_inquiry(msc_host_device_handle_t dev)
{
    msc_device_t *device = (msc_device_t *)dev;
    if (device == NULL || device->gone) return ESP_ERR_INVALID_STATE;
    cbw_inquiry_response_t response = {0};
    cbw_inquiry_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_inquiry_t), sizeof(response)),
        .opcode = SCSI_CMD_INQUIRY,
        .allocation_length = sizeof(response),
    };
    return bot_execute_command(device, &cbw.base, &response, sizeof(response));
}

esp_err_t scsi_cmd_mode_sense(msc_host_device_handle_t dev)
{
    msc_device_t *device = (msc_device_t *)dev;
    if (device == NULL || device->gone) return ESP_ERR_INVALID_STATE;
    mode_sense_response_t response = {0};
    mode_sense_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(mode_sense_t), sizeof(response)),
        .opcode = SCSI_CMD_MODE_SENSE,
        .pc_page_code = 0x3F,
        .parameter_list_length = sizeof(response),
    };
    return bot_execute_command(device, &cbw.base, &response, sizeof(response));
}

esp_err_t scsi_cmd_prevent_removal(msc_host_device_handle_t dev, bool prevent)
{
    msc_device_t *device = (msc_device_t *)dev;
    if (device == NULL || device->gone) return ESP_ERR_INVALID_STATE;
    prevent_allow_medium_removal_t cbw = {
        CBW_BASE_INIT(OUT_DIR, CBW_CMD_SIZE(prevent_allow_medium_removal_t), 0),
        .opcode = SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL,
        .prevent = (uint8_t)prevent,
    };
    return bot_execute_command(device, &cbw.base, NULL, 0);
}
