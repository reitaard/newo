#pragma once
#include <cstddef>
#include <cstdint>

namespace Newo2Storage {
bool begin();
bool available();
bool save_snapshot(const uint8_t *jpeg, size_t len, uint32_t sequence, char *out_path, size_t out_path_len);
bool begin_video(uint32_t sequence, char *out_path, size_t out_path_len);
bool append_video_frame(const uint8_t *jpeg, size_t len);
bool end_video(size_t *bytes_written);
bool video_open();
}  // namespace Newo2Storage
