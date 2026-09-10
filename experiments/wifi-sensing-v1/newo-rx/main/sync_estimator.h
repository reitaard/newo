#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sync_protocol.h"

typedef enum {
    NEWO_SYNC_UNSYNCED = 0, NEWO_SYNC_WARMING = 1, NEWO_SYNC_VALID = 2,
    NEWO_SYNC_DEGRADED = 3, NEWO_SYNC_STALE = 4,
} newo_sync_state_t;

typedef struct {
    uint32_t follower_session_id;
    uint32_t leader_node_id, leader_session_id;
    uint32_t retired_leader_sessions[4], retired_count;
    uint32_t last_sequence, accepted, rejected, transport_rx, transport_drops;
    uint64_t last_local_us, last_leader_us, anchor_local_us;
    int64_t raw_offset_us, smoothed_offset_us, anchor_offset_us;
    int32_t drift_milli_ppm;
    uint64_t residual_sum_sq;
} newo_sync_estimator_t;

void newo_sync_estimator_init(newo_sync_estimator_t *state,
                              uint32_t follower_session_id);
bool newo_sync_estimator_accept(newo_sync_estimator_t *state,
                                const newo_sync_beacon_t *beacon,
                                uint64_t local_receive_us);
newo_sync_state_t newo_sync_estimator_quality(const newo_sync_estimator_t *state,
                                               uint64_t now_us,
                                               uint32_t beacon_period_us);
uint32_t newo_sync_estimator_age(const newo_sync_estimator_t *state, uint64_t now_us);
uint32_t newo_sync_estimator_jitter(const newo_sync_estimator_t *state);
bool newo_sync_estimator_align(const newo_sync_estimator_t *state,
                               uint64_t local_us, uint64_t now_us,
                               uint32_t beacon_period_us,
                               int64_t *leader_us);
