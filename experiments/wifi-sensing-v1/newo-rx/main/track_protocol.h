#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NEWO_TRACK_CONTROL_SIZE 16u
typedef struct { uint32_t sequence; bool active; bool ack; } newo_track_control_t;
size_t newo_track_control_encode(const newo_track_control_t*, uint8_t*, size_t);
bool newo_track_control_decode(const uint8_t*, size_t, newo_track_control_t*);
