#include <Arduino.h>

#include "newo_audio.h"
#include "newo_cloud.h"
#include "newo_config.h"
#include "newo_display.h"
#include "newo_log.h"
#include "newo_speaker.h"
#include "newo_storage.h"
#include "newo_wifi.h"

NewoStorage newoStorage;
NewoWiFi newoWiFi(newoStorage);
NewoDisplay newoDisplay;
NewoCloud newoCloud(newoWiFi, newoDisplay);
NewoAudio newoAudio(newoWiFi, newoDisplay);
NewoSpeaker newoSpeaker(newoWiFi, newoDisplay, newoAudio);

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
  newoSpeaker.begin();

  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::BOOT, "BOOT_READY");
  newoCloud.recordStack("after boot");
}

void loop() {
  struct VoiceAck { char requestId[40]; };
  static VoiceAck pendingVoiceAcks[8] = {};
  static uint8_t pendingVoiceAckCount = 0;
  NewoCloud::VoiceRequest voiceRequest;
  NewoCloud::SpeakerRequest speakerRequest;
  NewoSpeaker::Result speakerResult;
  newoWiFi.loop();
  newoCloud.loop();
  // Consume every queued control request now. In particular, an OFF/toggle is
  // never held behind a prior request waiting for a streaming task to exit.
  while (newoCloud.consumeVoiceRequest(voiceRequest)) {
    bool enable = voiceRequest.action == NewoCloud::VoiceRequest::Action::ON;
    if (voiceRequest.action == NewoCloud::VoiceRequest::Action::TOGGLE) {
      // STREAMING is enabled for toggle purposes, so it is cancelled to OFF.
      enable = newoAudio.state() == NewoVoiceState::OFF;
    }
    newoAudio.setEnabled(enable);
    if (pendingVoiceAckCount < sizeof(pendingVoiceAcks) / sizeof(pendingVoiceAcks[0])) {
      strlcpy(pendingVoiceAcks[pendingVoiceAckCount++].requestId, voiceRequest.requestId,
              sizeof(pendingVoiceAcks[0].requestId));
    }
  }
  while (newoCloud.consumeSpeakerRequest(speakerRequest)) {
    NewoSpeaker::Request request = {};
    strlcpy(request.playbackId, speakerRequest.playbackId, sizeof(request.playbackId));
    request.sampleRate = speakerRequest.sampleRate;
    request.channels = speakerRequest.channels;
    request.bitsPerSample = speakerRequest.bitsPerSample;
    request.bytes = speakerRequest.bytes;
    if (!newoSpeaker.play(request)) newoCloud.sendSpeakerResult(request.playbackId, false, 0, "busy_or_invalid");
  }
  newoAudio.loop();
  newoSpeaker.loop();
  while (newoSpeaker.consumeResult(speakerResult)) {
    newoCloud.sendSpeakerResult(speakerResult.playbackId, speakerResult.success,
                                speakerResult.bytes, speakerResult.error);
  }
  newoCloud.updateVoiceTelemetry(newoAudio.state(), newoAudio.voiceConnected(), newoAudio.wakeCount(),
                                  newoAudio.sessionCount(), newoAudio.failures(), newoAudio.timeouts());
  if (!newoAudio.transitionPending()) {
    for (uint8_t i = 0; i < pendingVoiceAckCount; ++i) {
      newoCloud.sendVoiceAck(pendingVoiceAcks[i].requestId, newoAudio.state(), newoAudio.voiceConnected(),
                             newoAudio.wakeCount(), newoAudio.sessionCount());
    }
    pendingVoiceAckCount = 0;
  }
  newoDisplay.updateTelemetry(newoWiFi.connected(), newoWiFi.rssi(), newoCloud.connected(), millis(),
                              ESP.getFreeHeap(), ESP.getFreePsram(), NewoLog::stats());
  newoDisplay.loop();
  delay(2);
}
