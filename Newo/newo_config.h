#pragma once

#include <Arduino.h>

namespace NewoConfig {

constexpr char DEVICE_NAME[] = "Newo";
constexpr char FIRMWARE_VERSION[] = "0.2.0-dev";
constexpr char MDNS_HOST[] = "newo";
constexpr char SETUP_AP_SSID[] = "newo@ai.link";

constexpr char CLOUD_HOST[] = "newo.reitaard.de";
constexpr char CLOUD_PATH[] = "/device";
constexpr uint16_t CLOUD_PORT = 443;

constexpr uint8_t RGB_LED_PIN = 48;
constexpr size_t MAX_SAVED_NETWORKS = 8;

constexpr uint32_t INITIAL_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t RECONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t PORTAL_REBOOT_DELAY_MS = 1500;
constexpr uint32_t SETUP_AP_TIMEOUT_MS = 300000;

constexpr uint32_t CLOUD_RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t CLOUD_STATUS_INTERVAL_MS = 30000;
constexpr uint32_t CLOUD_WS_PING_INTERVAL_MS = 15000;
constexpr uint32_t CLOUD_WS_PONG_TIMEOUT_MS = 3000;
constexpr uint8_t CLOUD_WS_MISSED_PONG_LIMIT = 2;

}  // namespace NewoConfig
