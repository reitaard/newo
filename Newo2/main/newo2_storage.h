#pragma once
#include <cstddef>
#include <cstdint>

namespace Newo2Storage {
bool begin();
bool available();
bool save_snapshot(const uint8_t *jpeg, size_t len, uint32_t sequence, char *out_path, size_t out_path_len);
}  // namespace Newo2Storage
