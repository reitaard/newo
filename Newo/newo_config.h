#pragma once

#include <Arduino.h>

namespace NewoConfig {

constexpr char FIRMWARE_VERSION[] = "0.3.2-dev";
constexpr char PROVISIONING_DEVICE_NAME[] = "PROV_NEWO";

constexpr char CLOUD_HOST[] = "newo.reitaard.de";
constexpr char CLOUD_PATH[] = "/device";
constexpr uint16_t CLOUD_PORT = 443;

constexpr uint8_t RGB_LED_PIN = 48;
constexpr size_t MAX_SAVED_NETWORKS = 8;

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

}  // namespace NewoConfig
