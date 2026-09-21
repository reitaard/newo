#include "newo_time.h"

#include <Arduino.h>
#include <time.h>

namespace {
constexpr char kTimeZone[] = "ICT-7";  // UTC+7; POSIX TZ signs are reversed.
}

void NewoTime::begin() {
  configTzTime(kTimeZone, "pool.ntp.org", "time.nist.gov");
}

const char* NewoTime::zone() { return kTimeZone; }
