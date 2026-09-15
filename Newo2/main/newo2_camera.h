#pragma once
#include <cstddef>
#include <cstdint>

namespace Newo2Camera {
constexpr int kMotionWidth = 160;
constexpr int kMotionHeight = 120;
constexpr size_t kMotionPixels = kMotionWidth * kMotionHeight;

struct Snapshot {
    uint8_t *jpeg = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t sequence = 0;
};

bool begin();
bool set_enabled(bool enabled);
bool enabled();
bool capture_motion_luma(uint8_t *out, size_t out_len);
bool capture_snapshot(Snapshot &snapshot);
void release_snapshot(Snapshot &snapshot);
uint16_t sensor_pid();
}  // namespace Newo2Camera
