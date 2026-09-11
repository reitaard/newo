#include "sync_estimator.h"

#include <limits.h>
#include <math.h>
#include <string.h>

void newo_sync_estimator_init(newo_sync_estimator_t *s, uint32_t follower_session_id) {
    memset(s, 0, sizeof(*s)); s->follower_session_id = follower_session_id;
}

bool newo_sync_estimator_accept(newo_sync_estimator_t *s, const newo_sync_beacon_t *b,
                                uint64_t local_us) {
    s->transport_rx++;
    bool retired = false;
    if (b) for (uint32_t i = 0; i < s->retired_count; ++i)
        if (b->leader_session_id == s->retired_leader_sessions[i]) retired = true;
    if (!b || !b->leader_session_id || retired) {
        s->rejected++; return false;
    }
    if (s->leader_session_id && b->leader_session_id != s->leader_session_id) {
        uint32_t follower = s->follower_session_id, rx = s->transport_rx;
        uint32_t retired_sessions[4], retired_count = s->retired_count;
        memcpy(retired_sessions, s->retired_leader_sessions, sizeof(retired_sessions));
        if (retired_count < 4) retired_sessions[retired_count++] = s->leader_session_id;
        else { memmove(retired_sessions, retired_sessions + 1, 3 * sizeof(uint32_t)); retired_sessions[3] = s->leader_session_id; }
        newo_sync_estimator_init(s, follower); s->transport_rx = rx;
        memcpy(s->retired_leader_sessions, retired_sessions, sizeof(retired_sessions));
        s->retired_count = retired_count;
    }
    if (s->accepted && b->leader_session_id == s->leader_session_id &&
        b->sequence <= s->last_sequence) { s->rejected++; return false; }
    int64_t raw = (int64_t)b->leader_timestamp_us - (int64_t)local_us;
    if (!s->accepted) {
        s->smoothed_offset_us = raw; s->anchor_offset_us = raw; s->anchor_local_us = local_us;
    } else {
        int64_t residual = raw - s->smoothed_offset_us;
        s->smoothed_offset_us += residual / 8;
        uint64_t magnitude = (uint64_t)(residual < 0 ? -residual : residual);
        if (magnitude && magnitude <= UINT32_MAX && s->residual_sum_sq <= UINT64_MAX - magnitude * magnitude)
            s->residual_sum_sq += magnitude * magnitude;
        uint64_t elapsed = local_us - s->anchor_local_us;
        if (s->accepted >= 7 && elapsed) {
            int64_t drift = (raw - s->anchor_offset_us) * 1000000000LL / (int64_t)elapsed;
            if (drift > INT32_MAX) drift = INT32_MAX;
            if (drift < INT32_MIN) drift = INT32_MIN;
            s->drift_milli_ppm = s->accepted == 7 ? (int32_t)drift :
                s->drift_milli_ppm + ((int32_t)drift - s->drift_milli_ppm) / 8;
        }
    }
    s->leader_node_id = b->leader_node_id; s->leader_session_id = b->leader_session_id;
    s->last_sequence = b->sequence; s->last_local_us = local_us;
    s->last_leader_us = b->leader_timestamp_us; s->raw_offset_us = raw; s->accepted++;
    return true;
}

uint32_t newo_sync_estimator_age(const newo_sync_estimator_t *s, uint64_t now_us) {
    if (!s->accepted || now_us <= s->last_local_us) return s->accepted ? 0 : UINT32_MAX;
    uint64_t age = now_us - s->last_local_us; return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

newo_sync_state_t newo_sync_estimator_quality(const newo_sync_estimator_t *s,
                                               uint64_t now_us, uint32_t period) {
    if (!s->accepted) return NEWO_SYNC_UNSYNCED;
    uint32_t age = newo_sync_estimator_age(s, now_us);
    if (age > period * 10u) return NEWO_SYNC_STALE;
    if (s->accepted < 8u) return NEWO_SYNC_WARMING;
    if (age > period * 3u) return NEWO_SYNC_DEGRADED;
    return NEWO_SYNC_VALID;
}

uint32_t newo_sync_estimator_jitter(const newo_sync_estimator_t *s) {
    if (s->accepted < 2) return 0;
    double mean = (double)s->residual_sum_sq / (double)(s->accepted - 1u);
    double value = sqrt(mean); return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

bool newo_sync_estimator_align(const newo_sync_estimator_t *s, uint64_t local_us,
                               uint64_t now_us, uint32_t period, int64_t *leader_us) {
    newo_sync_state_t quality = newo_sync_estimator_quality(s, now_us, period);
    if (!leader_us || (quality != NEWO_SYNC_VALID && quality != NEWO_SYNC_DEGRADED)) return false;
    int64_t delta = local_us >= s->last_local_us
        ? (int64_t)(local_us - s->last_local_us)
        : -(int64_t)(s->last_local_us - local_us);
    *leader_us = (int64_t)s->last_leader_us + delta +
                 delta * s->drift_milli_ppm / 1000000000LL;
    return true;
}
