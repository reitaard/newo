#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
extern const uint8_t newo_alfred_model_start[];
extern const uint8_t newo_alfred_model_end[];
}

inline const uint8_t* newoAlfredModelData() { return newo_alfred_model_start; }
inline size_t newoAlfredModelSize() {
  return static_cast<size_t>(newo_alfred_model_end - newo_alfred_model_start);
}
