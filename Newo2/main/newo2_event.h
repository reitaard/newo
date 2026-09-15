#pragma once
#include <cstdint>

namespace Newo2Events {
enum class Type : uint8_t {
    CAMERA_SET,
    MOTION_SET,
    SNAPSHOT_REQUEST,
    STREAM_SET,
    RECORD_START,
    RECORD_STOP,
    SETTINGS_SET,
    STATUS_REQUEST,
    MOTION_DETECTED,
};

struct Event {
    Type type = Type::STATUS_REQUEST;
    bool enabled = false;
    float confidence = 0.0f;
    uint32_t sequence = 0;
    uint32_t duration_seconds = 0;
    char source[16] = {};
    char setting[16] = {};
    char value[16] = {};
    char request_id[40] = {};
};

bool begin();
bool publish(const Event &event, uint32_t timeout_ms = 0);
bool receive(Event &event, uint32_t timeout_ms);
uint32_t next_sequence();
}  // namespace Newo2Events
