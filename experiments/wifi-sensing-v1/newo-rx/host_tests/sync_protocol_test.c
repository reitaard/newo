#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../main/sync_estimator.h"

int main(void) {
    newo_sync_beacon_t beacon = {1, 0x12345678u, 1, 1000000, 3};
    uint8_t wire[NEWO_SYNC_BEACON_SIZE];
    assert(newo_sync_beacon_encode(&beacon, wire, sizeof(wire)) == sizeof(wire));
    newo_sync_beacon_t decoded;
    assert(newo_sync_beacon_decode(wire, sizeof(wire), &decoded));
    assert(decoded.leader_session_id == beacon.leader_session_id);
    wire[20] ^= 1; assert(!newo_sync_beacon_decode(wire, sizeof(wire), &decoded)); wire[20] ^= 1;

    newo_sync_estimator_t state;
    newo_sync_estimator_init(&state, 0x87654321u);
    for (uint32_t i = 1; i <= 12; ++i) {
        beacon.sequence = i; beacon.leader_timestamp_us = i * 500000ULL + 25000;
        assert(newo_sync_estimator_accept(&state, &beacon, i * 500000ULL));
    }
    assert(newo_sync_estimator_quality(&state, 6000000, 500000) == NEWO_SYNC_VALID);
    assert(state.smoothed_offset_us == 25000);
    assert(!newo_sync_estimator_accept(&state, &beacon, 6000000));
    assert(state.rejected == 1);
    assert(newo_sync_estimator_quality(&state, 8000000, 500000) == NEWO_SYNC_DEGRADED);
    assert(newo_sync_estimator_quality(&state, 12000000, 500000) == NEWO_SYNC_STALE);

    uint32_t old_session = beacon.leader_session_id;
    beacon.leader_session_id++; beacon.sequence = 1; beacon.leader_timestamp_us = 13000000;
    assert(newo_sync_estimator_accept(&state, &beacon, 12900000));
    assert(state.accepted == 1 && state.smoothed_offset_us == 100000);
    beacon.leader_session_id = old_session; beacon.sequence = 99;
    assert(!newo_sync_estimator_accept(&state, &beacon, 13000000));

    /* Slow drift, deterministic jitter, dropped sequence and one delay outlier. */
    newo_sync_estimator_init(&state, 77);
    beacon.leader_session_id = 88;
    for (uint32_t i = 1; i <= 30; ++i) {
        if (i == 10) continue;
        uint64_t local = i * 500000ULL;
        int64_t jitter = (i & 1) ? 100 : -100;
        int64_t delay_outlier = i == 20 ? 20000 : 0;
        beacon.sequence = i;
        beacon.leader_timestamp_us = local + 40000 + i * 5 + jitter + delay_outlier;
        assert(newo_sync_estimator_accept(&state, &beacon, local));
    }
    assert(state.accepted == 29);
    assert(state.drift_milli_ppm != 0);
    assert(newo_sync_estimator_jitter(&state) > 0);
    int64_t aligned;
    assert(newo_sync_estimator_align(&state, 16000000, 15000000,
                                     500000, &aligned));

    /* Follower reboot creates a new estimator epoch and must warm again. */
    newo_sync_estimator_init(&state, 78);
    assert(newo_sync_estimator_quality(&state, 0, 500000) == NEWO_SYNC_UNSYNCED);
    return 0;
}
