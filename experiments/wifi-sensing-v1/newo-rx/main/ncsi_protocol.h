/*
 * Newo CSI protocol v1 serializer.
 *
 * The byte layout is specified by ../PROTOCOL.md. This file is original Newo
 * code; measurement-plane lineage is documented in THIRD_PARTY_NOTICES.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NCSI_VERSION_MAJOR 1u
#define NCSI_RECORD_CSI 1u
#define NCSI_RECORD_STATUS 2u
#define NCSI_CSI_HEADER_SIZE 64u
#define NCSI_STATUS_RECORD_SIZE 80u
#define NCSI_MAX_CSI_BYTES 612u
#define NCSI_MAX_RECORD_SIZE (NCSI_CSI_HEADER_SIZE + NCSI_MAX_CSI_BYTES)

#define NCSI_FLAG_FIRST_WORD_INVALID_REPORTED (1u << 0)
#define NCSI_FLAG_INVALID_PREFIX_SANITIZED     (1u << 1)
#define NCSI_FLAG_SOURCE_FILTER_MATCHED        (1u << 2)
#define NCSI_FLAG_RX_METADATA_VALID            (1u << 3)
#define NCSI_FLAG_PAYLOAD_TRUNCATED            (1u << 4)
#define NCSI_FLAG_STBC                         (1u << 5)
#define NCSI_FLAG_CONTROL_TRAFFIC_ACTIVE       (1u << 6)
#define NCSI_FLAG_SEQUENCE_RESET               (1u << 7)

#define NCSI_STATUS_ASSOCIATED        (1u << 0)
#define NCSI_STATUS_CSI_ENABLED       (1u << 1)
#define NCSI_STATUS_FILTER_ENABLED    (1u << 2)
#define NCSI_STATUS_SELF_PING_ENABLED (1u << 3)
#define NCSI_STATUS_RING_HEALTHY      (1u << 4)

typedef struct {
    uint32_t node_id;
    uint8_t receiver_mac[6];
    uint8_t source_mac[6];
    uint32_t sequence;
    uint64_t timestamp_us;
    uint8_t channel;
    uint8_t secondary_channel;
    uint8_t bandwidth;
    uint8_t phy_mode;
    int8_t rssi_dbm;
    int8_t noise_floor_dbm;
    uint8_t antenna;
    uint8_t ltf_mask;
    uint16_t driver_csi_length;
    uint16_t csi_flags;
    uint16_t path_id;
    uint8_t sanitized_prefix_bytes;
    const uint8_t *csi;
    uint16_t csi_length;
} ncsi_csi_record_t;

typedef struct {
    uint32_t node_id;
    uint8_t receiver_mac[6];
    uint16_t status_flags;
    uint32_t boot_id;
    uint32_t status_sequence;
    uint64_t timestamp_us;
    uint32_t callbacks_total;
    uint32_t rate_gate_drops;
    uint32_t source_filter_drops;
    uint32_t accepted_total;
    uint32_t ring_full_drops;
    uint32_t transport_ok;
    uint32_t transport_drops;
    uint32_t last_csi_sequence;
    uint16_t raw_target_hz;
    uint16_t dsp_target_hz;
} ncsi_status_record_t;

uint32_t ncsi_crc32c(const uint8_t *data, size_t length);
uint8_t ncsi_sanitize_invalid_prefix(uint8_t *csi, uint16_t length,
                                     bool first_word_invalid);
size_t ncsi_serialize_csi(const ncsi_csi_record_t *record, uint8_t *output, size_t capacity);
size_t ncsi_serialize_status(const ncsi_status_record_t *record, uint8_t *output, size_t capacity);
