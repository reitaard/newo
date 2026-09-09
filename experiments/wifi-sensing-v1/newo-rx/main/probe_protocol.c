#include "probe_protocol.h"

#include "ncsi_protocol.h"

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void put_u64(uint8_t *dst, uint64_t value)
{
    put_u32(dst, (uint32_t)value);
    put_u32(dst + 4, (uint32_t)(value >> 32));
}

static uint16_t get_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint64_t get_u64(const uint8_t *src)
{
    return (uint64_t)get_u32(src) | ((uint64_t)get_u32(src + 4) << 32);
}

size_t newo_probe_encode(const newo_probe_t *probe, uint8_t *output,
                         size_t capacity)
{
    if (probe == NULL || output == NULL || capacity < NEWO_PROBE_SIZE) return 0;
    output[0] = 'N'; output[1] = 'P'; output[2] = 'R'; output[3] = 'B';
    output[4] = NEWO_PROBE_VERSION;
    output[5] = 0;
    put_u16(output + 6, NEWO_PROBE_SIZE);
    put_u32(output + 8, probe->sender_node_id);
    put_u32(output + 12, probe->sequence);
    put_u64(output + 16, probe->sender_timestamp_us);
    put_u32(output + 24, 0);
    put_u32(output + 24, ncsi_crc32c(output, NEWO_PROBE_SIZE));
    return NEWO_PROBE_SIZE;
}

bool newo_probe_decode(const uint8_t *input, size_t length,
                       newo_probe_t *probe)
{
    if (input == NULL || probe == NULL || length != NEWO_PROBE_SIZE ||
        input[0] != 'N' || input[1] != 'P' || input[2] != 'R' || input[3] != 'B' ||
        input[4] != NEWO_PROBE_VERSION || input[5] != 0 ||
        get_u16(input + 6) != NEWO_PROBE_SIZE) return false;
    uint8_t copy[NEWO_PROBE_SIZE];
    for (size_t i = 0; i < length; ++i) copy[i] = input[i];
    uint32_t expected = get_u32(copy + 24);
    put_u32(copy + 24, 0);
    if (ncsi_crc32c(copy, sizeof(copy)) != expected) return false;
    probe->sender_node_id = get_u32(input + 8);
    probe->sequence = get_u32(input + 12);
    probe->sender_timestamp_us = get_u64(input + 16);
    return true;
}
