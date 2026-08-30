#pragma once

#include <Arduino.h>

namespace NewoConfig {

constexpr char FIRMWARE_VERSION[] = "0.4.3-dev";
constexpr char PROVISIONING_DEVICE_NAME[] = "PROV_NEWO";

constexpr char CLOUD_HOST[] = "newo.reitaard.de";
constexpr char CLOUD_PATH[] = "/device";
constexpr char VOICE_PATH[] = "/voice";
constexpr char SPEAKER_PATH[] = "/speaker";
constexpr uint16_t CLOUD_PORT = 443;

constexpr uint8_t RGB_LED_PIN = 48;
constexpr size_t MAX_SAVED_NETWORKS = 8;

// ST7789 240x240 SPI display. VCC and BLK are wired to 3V3; GND to GND.
constexpr int8_t DISPLAY_SCK_PIN = 42;
constexpr int8_t DISPLAY_MOSI_PIN = 41;
constexpr int8_t DISPLAY_RST_PIN = 40;
constexpr int8_t DISPLAY_DC_PIN = 38;
constexpr int8_t DISPLAY_CS_PIN = 2;

// ESP32-S3 Dev Module audit: GPIO48 is the onboard RGB LED; flash/PSRAM,
// USB-JTAG (GPIO19/20), and bootstrap GPIO0 are deliberately avoided.
// GPIO4/5/6 are otherwise unused by Newo and are the proposed INMP441 wiring.
constexpr int8_t AUDIO_I2S_BCLK_PIN = 4;
constexpr int8_t AUDIO_I2S_WS_PIN = 5;
constexpr int8_t AUDIO_I2S_SD_PIN = 6;
constexpr bool AUDIO_I2S_MIC_IS_LEFT = true;  // Tie INMP441 L/R to GND.
constexpr uint32_t AUDIO_SAMPLE_RATE = 16'000;
constexpr uint16_t AUDIO_FRAME_DURATION_MS = 20;
constexpr size_t AUDIO_SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE * AUDIO_FRAME_DURATION_MS / 1'000;
constexpr size_t AUDIO_FRAME_BYTES = AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t);
// MAX98357A on a dedicated TX controller. Microphone mappings above are unchanged.
constexpr int8_t SPEAKER_I2S_BCLK_PIN = 21;
constexpr int8_t SPEAKER_I2S_WS_PIN = 47;
constexpr int8_t SPEAKER_I2S_DOUT_PIN = 14;
constexpr uint32_t SPEAKER_SAMPLE_RATE = 24'000;
constexpr uint32_t SPEAKER_PCM_BYTES_PER_SECOND = SPEAKER_SAMPLE_RATE * sizeof(int16_t);
constexpr size_t SPEAKER_CHUNK_BYTES = 2'048;
constexpr size_t SPEAKER_PREBUFFER_BYTES = 12'288;  // 256 ms mono PCM before audible output.
constexpr size_t SPEAKER_BUFFER_BYTES = 24'576;  // 512 ms mono PCM16, strictly bounded.

// Experimental WAN transport: each WebSocket binary frame carries one raw Opus
// packet inside an 8-byte Newo envelope (magic + sequence + valid PCM bytes).
// The final packet may be padded for Opus but only valid PCM bytes enter the
// decoded StreamBuffer, so completion still tracks the original speech exactly.
constexpr uint16_t SPEAKER_OPUS_BITRATE = 24'000;
constexpr uint16_t SPEAKER_OPUS_FRAME_MS = 40;
constexpr size_t SPEAKER_OPUS_FRAME_SAMPLES = SPEAKER_SAMPLE_RATE * SPEAKER_OPUS_FRAME_MS / 1'000;
constexpr size_t SPEAKER_OPUS_FRAME_PCM_BYTES = SPEAKER_OPUS_FRAME_SAMPLES * sizeof(int16_t);
constexpr size_t SPEAKER_OPUS_PACKET_HEADER_BYTES = 8;
// The server rejects envelopes above 4,000 bytes. Sixteen PSRAM slots provide
// 640 ms of bounded compressed-audio scheduling margin without a PCM reservoir.
constexpr size_t SPEAKER_OPUS_PACKET_MAX_BYTES = 4'000;
constexpr size_t SPEAKER_OPUS_QUEUE_DEPTH = 16;
constexpr uint32_t SPEAKER_OPUS_DECODER_STACK_BYTES = 8'192;
static_assert(SPEAKER_OPUS_FRAME_SAMPLES == 960, "speaker Opus frame samples changed");
static_assert(SPEAKER_OPUS_FRAME_PCM_BYTES == 1'920, "speaker Opus frame bytes changed");

// Delivery-aware flow reports are emitted from loop(), never the WebSocket
// callback or playback task. Counters remain decoded/original PCM bytes for
// both PCM and Opus transports, so the existing receiver-credit model stays
// physically meaningful while wire bandwidth drops sharply with Opus.
constexpr size_t SPEAKER_RECEIVE_REPORT_BYTES = 2'048;
constexpr size_t SPEAKER_CONSUME_REPORT_BYTES = 1'024;
constexpr size_t SPEAKER_LOW_WATER_BYTES = 10'240;
constexpr uint32_t SPEAKER_LOW_WATER_REPORT_INTERVAL_MS = 40;
// Arduino-ESP32 3.3.11 uses six 240-frame TX DMA descriptors. Waiting for one
// full ring plus one extra TX EOF after the final write makes completion mean
// the physical I2S tail has drained, rather than merely that our StreamBuffer is empty.
constexpr uint8_t SPEAKER_I2S_DRAIN_DMA_EVENTS = 7;
constexpr uint32_t SPEAKER_I2S_DRAIN_TIMEOUT_MS = 500;
static_assert(SPEAKER_PREBUFFER_BYTES <= SPEAKER_BUFFER_BYTES, "speaker prebuffer exceeds stream buffer");
static_assert(SPEAKER_CHUNK_BYTES <= SPEAKER_PREBUFFER_BYTES, "speaker chunk exceeds prebuffer");
static_assert(SPEAKER_RECEIVE_REPORT_BYTES <= SPEAKER_PREBUFFER_BYTES, "speaker receive report interval exceeds prebuffer");
static_assert(SPEAKER_CONSUME_REPORT_BYTES <= SPEAKER_PREBUFFER_BYTES, "speaker consume report interval exceeds prebuffer");
static_assert(SPEAKER_LOW_WATER_BYTES < SPEAKER_PREBUFFER_BYTES, "speaker low water must remain below prebuffer");
static_assert((SPEAKER_PREBUFFER_BYTES & 1) == 0, "speaker prebuffer must align to PCM16");
constexpr uint32_t SPEAKER_MAX_STREAM_BYTES = 2'880'000;  // At most 60 seconds.
constexpr uint32_t SPEAKER_STREAM_ABSOLUTE_TIMEOUT_MS = 75'000;
constexpr uint32_t SPEAKER_STREAM_NO_PROGRESS_TIMEOUT_MS = 10'000;
constexpr int16_t SPEAKER_DIGITAL_DIVISOR = 1;  // 100% digital amplitude; VPS limiter protects peaks.

constexpr uint32_t INITIAL_RECOVERY_WINDOW_MS = 18'000;
constexpr uint32_t WIFI_SCAN_RETRY_DELAY_MS = 1'000;
constexpr uint32_t WIFI_CONNECT_ATTEMPT_TIMEOUT_MS = 5'000;
constexpr uint32_t WIFI_CONNECT_POLL_DELAY_MS = 100;
constexpr uint32_t RECONNECT_INTERVAL_MS = 10'000;
constexpr uint32_t RECONNECT_WINDOW_MS = 6'000;
constexpr uint32_t BLE_PROVISIONING_TIMEOUT_MS = 300'000;
constexpr uint32_t PROVISIONING_REBOOT_DELAY_MS = 1'000;
constexpr uint32_t REMOTE_REBOOT_DELAY_MS = 1'000;

constexpr uint32_t CLOUD_RECONNECT_INTERVAL_MS = 5'000;
constexpr uint32_t CLOUD_STATUS_INTERVAL_MS = 30'000;
constexpr uint32_t CLOUD_WS_PING_INTERVAL_MS = 15'000;
constexpr uint32_t CLOUD_WS_PONG_TIMEOUT_MS = 3'000;
constexpr uint8_t CLOUD_WS_MISSED_PONG_LIMIT = 2;
// Voice defaults OFF for the first ESP_SR physical bring-up. ARMED is local-only;
// STREAMING has a finite lifetime and must never retain stale PCM.
constexpr bool VOICE_DEFAULT_ENABLED = false;
constexpr uint32_t VOICE_ACTIVE_SESSION_TIMEOUT_MS = 30'000;

}  // namespace NewoConfig
