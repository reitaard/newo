#include "rate_gate.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    newo_rate_gate_t gate = {0};
    uint32_t sequence = 0;
    uint32_t retained[3] = {0};
    uint32_t drops[3] = {0};
    uint32_t assigned[4] = {0};
    size_t assigned_count = 0;

    /* Router path 1 arrives at 1 kHz; Newo2 path 3 arrives at 20 Hz. Each path
     * independently retains at 20 Hz, and accepted sequences stay node-global. */
    for (int64_t now = 1000; now <= 151000; now += 1000) {
        if (newo_rate_gate_accept(&gate, 1, now, 50000)) {
            retained[0]++;
            if (assigned_count < 4) assigned[assigned_count++] = sequence;
            sequence++;
        } else {
            drops[0]++;
        }
        if (now == 1000 || now == 51000 || now == 101000 || now == 151000) {
            assert(newo_rate_gate_accept(&gate, 3, now, 50000));
            retained[2]++;
            if (assigned_count < 4) assigned[assigned_count++] = sequence;
            sequence++;
        }
    }
    assert(retained[0] == 4);
    assert(retained[2] == 4);
    assert(drops[0] == 147);
    assert(drops[2] == 0);
    assert(sequence == 8);
    assert(assigned[0] == 0 && assigned[1] == 1);
    assert(assigned[2] == 2 && assigned[3] == 3);

    /* A too-early path-3 frame drops without affecting path 1 or sequence. */
    assert(!newo_rate_gate_accept(&gate, 3, 160000, 50000));
    drops[2]++;
    assert(sequence == 8 && drops[2] == 1);
    puts("per-path rate gate tests passed");
    return 0;
}
