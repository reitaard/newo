#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "track_protocol.h"
typedef enum { NEWO_TRACK_APPLIED, NEWO_TRACK_DUPLICATE, NEWO_TRACK_STALE, NEWO_TRACK_RETIRED_SESSION } newo_track_apply_result_t;
typedef struct { uint32_t session_id, retired_session_id, sequence; bool active; } newo_track_control_state_t;
newo_track_apply_result_t newo_track_control_apply(newo_track_control_state_t*, const newo_track_control_t*);
