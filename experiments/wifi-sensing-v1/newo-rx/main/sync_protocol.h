#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NEWO_SYNC_VERSION 1u
#define NEWO_SYNC_BEACON_TYPE 1u
#define NEWO_SYNC_BEACON_SIZE 36u

typedef struct {
    uint32_t leader_node_id;
    uint32_t leader_session_id;
    uint32_t sequence;
    uint64_t leader_timestamp_us;
    uint32_t capability_flags;
} newo_sync_beacon_t;

size_t newo_sync_beacon_encode(const newo_sync_beacon_t *beacon,
                               uint8_t *output, size_t capacity);
bool newo_sync_beacon_decode(const uint8_t *wire, size_t length,
                             newo_sync_beacon_t *beacon);
