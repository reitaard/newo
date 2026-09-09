#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NEWO_PROBE_VERSION 1u
#define NEWO_PROBE_SIZE 28u

typedef struct {
    uint32_t sender_node_id;
    uint32_t sequence;
    uint64_t sender_timestamp_us;
} newo_probe_t;

size_t newo_probe_encode(const newo_probe_t *probe, uint8_t *output,
                         size_t capacity);
bool newo_probe_decode(const uint8_t *input, size_t length,
                       newo_probe_t *probe);
