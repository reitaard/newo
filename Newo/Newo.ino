#include <Arduino.h>

#include "newo_audio.h"
#include "newo_cloud.h"
#include "newo_config.h"
#include "newo_display.h"
#include "newo_log.h"
#include "newo_storage.h"
#include "newo_wifi.h"

NewoStorage newoStorage;
NewoWiFi newoWiFi(newoStorage);
NewoDisplay newoDisplay;
NewoCloud newoCloud(newoWiFi, newoDisplay);
NewoAudio newoAudio(newoWiFi, newoDisplay);

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
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::BOOT, "BOOT_START");
  newoDisplay.begin();

  if (!newoStorage.begin()) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::STORAGE, "STORAGE_FAILED");
  } else {
    char detail[48];
    snprintf(detail, sizeof(detail), "saved_networks=%u", static_cast<unsigned>(newoStorage.count()));
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::STORAGE, "STORAGE_READY", detail);
  }

  newoWiFi.begin();
  newoCloud.begin();
  newoAudio.begin();

  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::BOOT, "BOOT_READY");
  newoCloud.recordStack("after boot");
}

void loop() {
  static bool voiceRequestWaiting = false;
  static NewoCloud::VoiceRequest voiceRequest;
  newoWiFi.loop();
  newoCloud.loop();
  if (!voiceRequestWaiting && newoCloud.consumeVoiceRequest(voiceRequest)) {
    if (voiceRequest.action == NewoCloud::VoiceRequest::Action::ON) newoAudio.setEnabled(true);
    else if (voiceRequest.action == NewoCloud::VoiceRequest::Action::OFF) newoAudio.setEnabled(false);
    voiceRequestWaiting = true;
  }
  newoAudio.loop();
  newoCloud.updateVoiceTelemetry(newoAudio.state(), newoAudio.voiceConnected(), newoAudio.wakeCount(),
                                  newoAudio.sessionCount(), newoAudio.failures(), newoAudio.timeouts());
  if (voiceRequestWaiting && !newoAudio.transitionPending()) {
    newoCloud.sendVoiceAck(voiceRequest.requestId, newoAudio.state(), newoAudio.voiceConnected(),
                           newoAudio.wakeCount(), newoAudio.sessionCount());
    voiceRequestWaiting = false;
  }
  newoDisplay.updateTelemetry(newoWiFi.connected(), newoWiFi.rssi(), newoCloud.connected(), millis(),
                              ESP.getFreeHeap(), ESP.getFreePsram(), NewoLog::stats());
  newoDisplay.loop();
  delay(2);
}
