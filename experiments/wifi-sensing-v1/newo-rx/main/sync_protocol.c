#include "sync_protocol.h"

#include <string.h>

#include "ncsi_protocol.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put64(uint8_t *p, uint64_t v) { put32(p, v); put32(p + 4, v >> 32); }
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

size_t newo_sync_beacon_encode(const newo_sync_beacon_t *b, uint8_t *out, size_t capacity) {
    if (!b || !out || capacity < NEWO_SYNC_BEACON_SIZE || !b->leader_session_id) return 0;
    memset(out, 0, NEWO_SYNC_BEACON_SIZE);
    memcpy(out, "NSYN", 4); out[4] = NEWO_SYNC_VERSION; out[5] = NEWO_SYNC_BEACON_TYPE;
    put16(out + 6, NEWO_SYNC_BEACON_SIZE); put32(out + 8, b->leader_node_id);
    put32(out + 12, b->leader_session_id); put32(out + 16, b->sequence);
    put64(out + 20, b->leader_timestamp_us); put32(out + 28, b->capability_flags);
    put32(out + 32, ncsi_crc32c(out, 32));
    return NEWO_SYNC_BEACON_SIZE;
}

bool newo_sync_beacon_decode(const uint8_t *wire, size_t length, newo_sync_beacon_t *b) {
    if (!wire || !b || length != NEWO_SYNC_BEACON_SIZE || memcmp(wire, "NSYN", 4) ||
        wire[4] != NEWO_SYNC_VERSION || wire[5] != NEWO_SYNC_BEACON_TYPE ||
        get16(wire + 6) != NEWO_SYNC_BEACON_SIZE || !get32(wire + 12) ||
        get32(wire + 32) != ncsi_crc32c(wire, 32)) return false;
    b->leader_node_id = get32(wire + 8); b->leader_session_id = get32(wire + 12);
    b->sequence = get32(wire + 16); b->leader_timestamp_us = get64(wire + 20);
    b->capability_flags = get32(wire + 28); return true;
}
