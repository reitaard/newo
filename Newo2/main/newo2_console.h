#pragma once

#include <cstddef>
#include <cstdint>

namespace Newo2Console {
bool begin();
bool set_remote_enabled(bool enabled);
bool remote_enabled();
size_t read_remote(uint8_t *destination, size_t capacity, uint32_t *dropped_bytes);
void note_remote_drop(size_t bytes);
constexpr size_t remote_capacity() { return 32 * 1024; }
}  // namespace Newo2Console
