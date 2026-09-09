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

    puts("ncsi protocol tests passed");
    return 0;
}
