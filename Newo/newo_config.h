#pragma once

#include <Arduino.h>

namespace NewoConfig {

constexpr char DEVICE_NAME[] = "Newo";
constexpr char MDNS_HOST[] = "newo";
constexpr char SETUP_AP_SSID[] = "newo@ai.link";

constexpr uint8_t RGB_LED_PIN = 48;
constexpr size_t MAX_SAVED_NETWORKS = 8;

constexpr uint32_t INITIAL_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t RECONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t PORTAL_REBOOT_DELAY_MS = 1500;

}  // namespace NewoConfig
