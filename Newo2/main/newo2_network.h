#pragma once
#include <cstddef>
#include <cstdint>

namespace Newo2Network {
bool begin();
bool cloud_connected();
void send_control_ack(const char *request_id, const char *target, bool enabled, bool applied);
void send_status(bool camera_enabled, bool motion_enabled);
void send_motion_detected(uint32_t sequence, float confidence);
void send_snapshot_result(const char *request_id, const char *source, uint32_t sequence,
                          size_t bytes, bool saved, bool uploaded, bool captured);
bool upload_snapshot(const uint8_t *jpeg, size_t len, const char *request_id,
                     const char *source, uint32_t sequence);
}  // namespace Newo2Network
