#include "rate_gate.h"

bool newo_rate_gate_accept(newo_rate_gate_t *gate, uint16_t path_id,
                           int64_t now_us, int64_t interval_us)
{
    if (gate == 0 || path_id == 0 || path_id > NEWO_PATH_COUNT ||
        now_us < 0 || interval_us <= 0) {
        return false;
    }
    int64_t *last = &gate->last_accepted_us[path_id - 1u];
    if (*last != 0 && now_us - *last < interval_us) return false;
    *last = now_us;
    return true;
}
