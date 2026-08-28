#pragma once

#include <Arduino.h>

namespace NewoConfig {

constexpr char FIRMWARE_VERSION[] = "0.3.2-dev";
constexpr char PROVISIONING_DEVICE_NAME[] = "PROV_NEWO";

constexpr char CLOUD_HOST[] = "newo.reitaard.de";
constexpr char CLOUD_PATH[] = "/device";
constexpr char VOICE_PATH[] = "/voice";
constexpr uint16_t CLOUD_PORT = 443;

constexpr uint8_t RGB_LED_PIN = 48;
constexpr size_t MAX_SAVED_NETWORKS = 8;

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
// Fixed digital gain for the physical microphone test. Applied to signed PCM24
// before the final PCM16 reduction; raise only after reviewing clip metrics.
constexpr uint8_t AUDIO_MIC_GAIN = 4;
// 24 frames retain at most 480 ms. Bounded sender draining catches short loop
// stalls without letting audio become unboundedly stale.
constexpr size_t AUDIO_QUEUE_DEPTH = 24;
constexpr size_t AUDIO_SEND_DRAIN_LIMIT = 6;
constexpr uint32_t AUDIO_LEVEL_LOG_INTERVAL_MS = 5'000;

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
constexpr uint32_t VOICE_WS_RECONNECT_INTERVAL_MS = 5'000;

}  // namespace NewoConfig
