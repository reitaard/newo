#pragma once
#include <cstddef>
#include <cstdint>

namespace Newo2Network {
bool begin();
bool cloud_connected();
void send_control_ack(const char *request_id, const char *target, bool enabled, bool applied);
void send_status(bool camera_enabled, bool motion_enabled);
void send_motion_detected(uint32_t sequence, float confidence);
void send_serial_monitor_ack(const char *request_id, bool enabled, bool applied);
void send_media_ack(const char *request_id, const char *target, bool enabled, bool applied, uint8_t fps);
void send_camera_settings(const char *request_id, bool applied);
void send_record_result(const char *request_id, bool success, uint32_t frames, uint32_t dropped,
                        size_t bytes, uint32_t duration_ms, const char *reason);
bool send_video_frame(const uint8_t *jpeg, size_t len, uint16_t width, uint16_t height,
                      uint32_t sequence, uint8_t fps, bool streaming, bool recording);
void send_snapshot_result(const char *request_id, const char *source, uint32_t sequence,
                          size_t bytes, bool saved, bool uploaded, bool captured);
bool upload_snapshot(const uint8_t *jpeg, size_t len, const char *request_id,
                     const char *source, uint32_t sequence);
}  // namespace Newo2Network
