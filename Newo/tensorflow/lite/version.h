#pragma once

// Arduino-ESP32 3.3.10 exposes the ESP-TFLM schema headers but not the upstream
// tensorflow/lite/version.h convenience header used by newer examples. TFLite
// schema version 3 is the version carried by the bundled runtime and by Alfred.
#ifndef TFLITE_SCHEMA_VERSION
#define TFLITE_SCHEMA_VERSION 3
#endif
