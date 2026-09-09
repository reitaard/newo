#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NEWO_PATH_COUNT 3u

typedef struct {
    int64_t last_accepted_us[NEWO_PATH_COUNT];
} newo_rate_gate_t;

bool newo_rate_gate_accept(newo_rate_gate_t *gate, uint16_t path_id,
                           int64_t now_us, int64_t interval_us);
