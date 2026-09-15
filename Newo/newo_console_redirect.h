#pragma once

#include "newo_console.h"

// Include only after Arduino/framework headers in Newo-owned translation units.
// It redirects application Serial output through the nonblocking remote console
// tap while preserving the physical USB CDC destination.
#define Serial NewoConsole
