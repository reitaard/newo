#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ncsi_protocol.h"

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void verify_record_crc(uint8_t *wire, size_t length)
{
    uint32_t expected = get_u32(wire + 12);
    memset(wire + 12, 0, 4);
    assert(ncsi_crc32c(wire, length) == expected);
}

int main(void)
{
    static const uint8_t check[] = "123456789";
    assert(ncsi_crc32c(check, sizeof(check) - 1) == 0xE3069283u);

    uint8_t iq[] = {11, 12, 13, 14, 21, 22, 31, 32};
    assert(ncsi_sanitize_invalid_prefix(iq, sizeof(iq), true) == 4);
    assert(iq[0] == 0 && iq[1] == 0 && iq[2] == 0 && iq[3] == 0);
    assert(iq[4] == 21 && iq[5] == 22 && iq[6] == 31 && iq[7] == 32);

    ncsi_csi_record_t record = {
        .node_id = 0x11223344u,
        .receiver_mac = {0x02, 0, 0, 0, 0, 1},
        .source_mac = {0x02, 0, 0, 0, 0, 2},
        .sequence = 7,
        .timestamp_us = 123456789,
        .channel = 6,
        .secondary_channel = 1,
        .bandwidth = 1,
        .phy_mode = 1,
        .rssi_dbm = -42,
        .noise_floor_dbm = -96,
        .antenna = 0,
        .ltf_mask = 3,
        .driver_csi_length = sizeof(iq),
        .csi_flags = NCSI_FLAG_FIRST_WORD_INVALID_REPORTED |
                     NCSI_FLAG_INVALID_PREFIX_SANITIZED,
        .path_id = 1,
        .sanitized_prefix_bytes = 4,
        .driver_rx_timestamp_us = 0x55667788u,
        .phy_rate = 11,
        .mcs = 5,
        .rx_flags = NCSI_RX_FLAG_MCS_VALID | NCSI_RX_FLAG_SHORT_GI,
        .ampdu_count = 3,
        .rx_state = 0,
        .packet_length = 128,
        .driver_rx_sequence = 0x3344,
        .destination_mac = {0x02, 0, 0, 0, 0, 3},
        .csi = iq,
        .csi_length = sizeof(iq),
    };
    uint8_t wire[NCSI_MAX_RECORD_SIZE];
    size_t length = ncsi_serialize_csi(&record, wire, sizeof(wire));
    assert(length == NCSI_CSI_HEADER_SIZE + sizeof(iq));
    assert(memcmp(wire, "NCSI", 4) == 0);
    assert(wire[4] == 1 && wire[5] == NCSI_RECORD_CSI);
    assert(get_u16(wire + 6) == NCSI_CSI_HEADER_SIZE);
    assert(get_u32(wire + 8) == length);
    assert(get_u32(wire + 16) == 0x11223344u);
    assert(get_u16(wire + 52) == sizeof(iq));
    assert(get_u16(wire + 54) == sizeof(iq));
    assert(get_u16(wire + 56) == sizeof(iq) / 2);
    assert(wire[62] == 4 && wire[63] == 1);
    assert(get_u32(wire + 64) == 0x55667788u);
    assert(wire[68] == 11 && wire[69] == 5);
    assert(get_u16(wire + 70) == (NCSI_RX_FLAG_MCS_VALID | NCSI_RX_FLAG_SHORT_GI));
    assert(wire[72] == 3 && wire[73] == 0);
    assert(get_u16(wire + 74) == 128);
    assert(get_u16(wire + 76) == 0x3344);
    assert(memcmp(wire + 80, record.destination_mac, 6) == 0);
    assert(memcmp(wire + NCSI_CSI_HEADER_SIZE, iq, sizeof(iq)) == 0);
    verify_record_crc(wire, length);

    uint8_t odd_iq[] = {1, 2, 3};
    record.csi = odd_iq;
    record.csi_length = sizeof(odd_iq);
    assert(ncsi_serialize_csi(&record, wire, sizeof(wire)) == 0);

    record.csi = iq;
    record.csi_length = sizeof(iq);
    record.driver_csi_length = sizeof(iq) - 2;
    assert(ncsi_serialize_csi(&record, wire, sizeof(wire)) == 0);

    record.driver_csi_length = sizeof(iq);
    iq[0] = 1;
    assert(ncsi_serialize_csi(&record, wire, sizeof(wire)) == 0);

    ncsi_diagnostic_record_t diagnostic = {
        .node_id = 7,
        .receiver_mac = {1, 2, 3, 4, 5, 6},
        .diagnostic_flags = NCSI_STATUS_ASSOCIATED,
        .boot_id = 8,
        .diagnostic_sequence = 9,
        .timestamp_us = 10,
        .status_transport_ok = 11,
        .status_transport_drops = 12,
        .probe_tx_attempted = 13,
        .probe_tx_queued = 14,
        .probe_tx_success = 15,
        .probe_tx_link_failure = 16,
        .probe_tx_submit_failure = 17,
        .probe_tx_skipped_busy = 18,
        .probe_tx_skipped_unassociated = 19,
        .probe_rx_valid = 20,
        .probe_rx_invalid = 21,
        .path_gate_drops = {22, 23, 24},
        .association_epoch = 25,
    };
    length = ncsi_serialize_diagnostic(&diagnostic, wire, sizeof(wire));
    assert(length == NCSI_DIAGNOSTIC_RECORD_SIZE);
    assert(wire[5] == NCSI_RECORD_DIAGNOSTIC);
    assert(get_u32(wire + 52) == 13);
    assert(get_u32(wire + 64) == 16);
    assert(get_u32(wire + 88) == 22);
    assert(get_u32(wire + 100) == 25);
    verify_record_crc(wire, length);

    ncsi_sync_record_t sync = {
        .node_id = 2, .receiver_mac = {2, 2, 2, 2, 2, 2},
        .sync_version = 1, .sync_state = 2, .boot_id = 10,
        .sync_sequence = 11, .local_timestamp_us = 1000000,
        .leader_timestamp_us = 1025000, .raw_offset_us = 25000,
        .smoothed_offset_us = 24900, .drift_milli_ppm = -1250,
        .accepted_samples = 12, .rejected_samples = 1,
        .last_sync_age_us = 500000, .leader_node_id = 1,
        .leader_session_id = 20, .follower_session_id = 21,
        .beacon_sequence = 12, .jitter_us = 80,
        .transport_rx = 13, .transport_drops = 2,
    };
    length = ncsi_serialize_sync(&sync, wire, sizeof(wire));
    assert(length == NCSI_SYNC_RECORD_SIZE && wire[5] == NCSI_RECORD_SYNC);
    assert(wire[26] == 1 && wire[27] == 2);
    assert(get_u32(wire + 68) == (uint32_t)-1250);
    assert(get_u32(wire + 88) == 20 && get_u32(wire + 108) == 2);
    verify_record_crc(wire, length);

    puts("ncsi protocol tests passed");
    return 0;
}
