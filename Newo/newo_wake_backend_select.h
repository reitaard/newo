#pragma once

// Alfred is the physical-test default on this branch. Set this to 0 (or pass
// -DNEWO_USE_ALFRED_MWW=0) to compile the existing ESP-SR / Hi Wall-E backend
// instead without changing NewoAudio's ownership/session logic.
#ifndef NEWO_USE_ALFRED_MWW
#define NEWO_USE_ALFRED_MWW 1
#endif

#if NEWO_USE_ALFRED_MWW
#include "newo_mww_engine.h"
using NewoActiveWakeEngine = NewoMicroWakeEngine;
#else
#include "newo_wake_engine.h"
using NewoActiveWakeEngine = NewoEspSrWakeEngine;
#endif
