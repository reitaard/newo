#include "probe_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    newo_probe_t input = {
        .sender_node_id = 2,
        .sequence = 0x10203040,
        .sender_timestamp_us = UINT64_C(0x0102030405060708),
    };
    uint8_t wire[NEWO_PROBE_SIZE];
    assert(newo_probe_encode(&input, wire, sizeof(wire)) == sizeof(wire));
    assert(memcmp(wire, "NPRB", 4) == 0);
    assert(wire[4] == NEWO_PROBE_VERSION);
    assert(wire[6] == NEWO_PROBE_SIZE && wire[7] == 0);

    newo_probe_t output = {0};
    assert(newo_probe_decode(wire, sizeof(wire), &output));
    assert(output.sender_node_id == input.sender_node_id);
    assert(output.sequence == input.sequence);
    assert(output.sender_timestamp_us == input.sender_timestamp_us);

    wire[12] ^= 1;
    assert(!newo_probe_decode(wire, sizeof(wire), &output));
    assert(!newo_probe_decode(wire, sizeof(wire) - 1, &output));
    puts("probe protocol tests passed");
    return 0;
}
