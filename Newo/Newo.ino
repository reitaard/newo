#include <Arduino.h>

#include "newo_cloud.h"
#include "newo_config.h"
#include "newo_storage.h"
#include "newo_wifi.h"

NewoStorage newoStorage;
NewoWiFi newoWiFi(newoStorage);
NewoCloud newoCloud(newoWiFi);

void printHardwareInfo() {
  Serial.println();
  Serial.println("================================");
  Serial.println("            NEWO");
  Serial.println("================================");
  Serial.printf("Firmware: %s\n", NewoConfig::FIRMWARE_VERSION);
  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("CPU: %lu MHz\n", static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  Serial.printf("Flash: %lu MB\n", static_cast<unsigned long>(ESP.getFlashChipSize() / 1024 / 1024));
  Serial.printf("PSRAM: %lu MB\n", static_cast<unsigned long>(ESP.getPsramSize() / 1024 / 1024));
  Serial.printf("Free PSRAM: %lu MB\n", static_cast<unsigned long>(ESP.getFreePsram() / 1024 / 1024));
  Serial.println("================================");
}

void setup() {
  // Keep the confirmed onboard RGB LED dark.
  rgbLedWrite(NewoConfig::RGB_LED_PIN, 0, 0, 0);

  Serial.begin(115200);
  delay(1200);
  printHardwareInfo();

  if (!newoStorage.begin()) {
    Serial.println("[boot] Storage initialization failed");
  }

  newoWiFi.begin();
  newoCloud.begin();

  Serial.println("[boot] Newo ready");
}

void loop() {
  newoWiFi.loop();
  newoCloud.loop();
  delay(2);
}
