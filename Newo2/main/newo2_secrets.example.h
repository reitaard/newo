#pragma once

// Copy this file to Newo2/main/newo2_secrets.h. That local file is git-ignored.
// Use a unique Newo2 device credential on the VPS; do not reuse Newo's secret.
namespace Newo2Secrets {
inline constexpr char WIFI_SSID[] = "";
inline constexpr char WIFI_PASSWORD[] = "";
inline constexpr char DEVICE_ID[] = "newo2-01";
inline constexpr char DEVICE_SECRET[] = "";
inline constexpr char CLOUD_HOST[] = "newo.reitaard.de";
}  // namespace Newo2Secrets
