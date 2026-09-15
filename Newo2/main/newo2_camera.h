#pragma once
#include <cstddef>
#include <cstdint>

namespace Newo2Camera {
struct Snapshot {
    uint8_t *jpeg = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t sequence = 0;
};

struct Settings {
    const char *photo_resolution;
    const char *video_resolution;
    uint8_t photo_quality;
    uint8_t video_quality;
};

bool begin();
bool set_enabled(bool enabled);
bool enabled();
bool capture_snapshot(Snapshot &snapshot);
bool capture_video_frame(Snapshot &frame);
Settings settings();
bool set_resolution(const char *target, const char *resolution);
bool set_quality(const char *target, uint8_t quality);
void release_snapshot(Snapshot &snapshot);
uint16_t sensor_pid();
}  // namespace Newo2Camera
