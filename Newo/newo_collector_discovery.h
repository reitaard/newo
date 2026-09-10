#pragma once
#include <Arduino.h>

struct NewoCollectorDestination { uint32_t address; uint16_t port; const char* source; };
bool newoCollectorDiscoveryStart(const char* fallback, uint16_t port);
void newoCollectorDiscoveryStop();
NewoCollectorDestination newoCollectorSnapshot();
