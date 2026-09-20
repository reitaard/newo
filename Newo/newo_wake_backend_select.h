#pragma once

#include "newo_mww_engine.h"

// Alfred is the only local wake backend. The former ESP-SR / Hi Wall-E
// fallback has been removed from the firmware.
using NewoActiveWakeEngine = NewoMicroWakeEngine;
