#include <Arduino.h>

#include "newo_audio.h"
#include "newo_cloud.h"
#include "newo_config.h"
#include "newo_display.h"
#include "newo_log.h"
#include "newo_led.h"
#include "newo_speaker.h"
#include "newo_storage.h"
#include "newo_wifi.h"

NewoStorage newoStorage;
NewoWiFi newoWiFi(newoStorage);
NewoLed newoLed;
NewoDisplay newoDisplay;
NewoCloud newoCloud(newoWiFi, newoDisplay);
NewoAudio newoAudio(newoWiFi, newoDisplay);
NewoSpeaker newoSpeaker(newoWiFi, newoDisplay, newoAudio, newoStorage);

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
  Serial.begin(115200);
  delay(1200);
  newoLed.begin();
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
  struct VoiceAck { char requestId[40]; bool applied; };
  struct SpeakerAck { char requestId[40]; bool targetEnabled; bool applied; uint32_t startedMs; };
  static VoiceAck pendingVoiceAcks[8] = {};
  static uint8_t pendingVoiceAckCount = 0;
  static SpeakerAck pendingSpeakerAcks[4] = {};
  static uint8_t pendingSpeakerAckCount = 0;
  NewoCloud::VoiceRequest voiceRequest;
  NewoCloud::SpeakerControlRequest speakerControlRequest;
  NewoSpeaker::PlaybackStarted speakerStarted;
  NewoSpeaker::Result speakerResult;
  newoWiFi.loop();
  newoCloud.loop();
  newoLed.setProvisioning(newoWiFi.provisioningActive());
  newoLed.setConnectivity(newoWiFi.connected() && newoCloud.connected());
  switch (newoWiFi.consumeLedEvent()) {
    case NewoWiFi::LedEvent::ACCEPTED: newoLed.flashProvisioningAccepted(); break;
    case NewoWiFi::LedEvent::REJECTED: newoLed.flashProvisioningRejected(); break;
    case NewoWiFi::LedEvent::SAVED: newoLed.flashProvisioningSaved(); break;
    case NewoWiFi::LedEvent::TIMEOUT: newoLed.flashProvisioningTimeout(); break;
    case NewoWiFi::LedEvent::NONE: break;
  }
  switch (newoCloud.consumeLedEvent()) {
    case NewoCloud::LedEvent::PING: newoLed.flashPing(); break;
    case NewoCloud::LedEvent::REBOOT: newoLed.startRebootSequence(); break;
    case NewoCloud::LedEvent::NONE: break;
  }
  // Consume every queued control request now. In particular, an OFF/toggle is
  // never held behind a prior request waiting for a streaming task to exit.
  while (newoCloud.consumeVoiceRequest(voiceRequest)) {
    bool applied = true;
    if (voiceRequest.action == NewoCloud::VoiceRequest::Action::MANUAL_TOGGLE) {
      // Manual /v is OFF -> STREAMING and STREAMING -> OFF. It never arms
      // WakeNet and never queues behind speaker playback.
      applied = newoAudio.manualToggle();
    } else {
      bool enable = voiceRequest.action == NewoCloud::VoiceRequest::Action::ON;
      if (voiceRequest.action == NewoCloud::VoiceRequest::Action::TOGGLE) {
        // Preserve the legacy/future WakeNet toggle semantics.
        enable = newoAudio.state() == NewoVoiceState::OFF;
      }
      applied = newoAudio.setEnabled(enable);
    }
    if (pendingVoiceAckCount < sizeof(pendingVoiceAcks) / sizeof(pendingVoiceAcks[0])) {
      VoiceAck& pending = pendingVoiceAcks[pendingVoiceAckCount++];
      strlcpy(pending.requestId, voiceRequest.requestId, sizeof(pending.requestId));
      pending.applied = applied;
    }
  }
  while (newoCloud.consumeSpeakerControlRequest(speakerControlRequest)) {
    bool applied = true;
    bool deferAck = false;
    if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_VOLUME) {
      applied = newoSpeaker.setVolume(speakerControlRequest.volume);
    } else if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::TOGGLE_MUTE) {
      applied = newoSpeaker.setMuted(!newoSpeaker.muted());
    } else if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_ENABLED) {
      applied = newoSpeaker.setEnabled(speakerControlRequest.enabled);
      const bool complete = speakerControlRequest.enabled ? newoSpeaker.ready() : newoSpeaker.released();
      if (applied && !complete) {
        if (pendingSpeakerAckCount < sizeof(pendingSpeakerAcks) / sizeof(pendingSpeakerAcks[0])) {
          SpeakerAck& pending = pendingSpeakerAcks[pendingSpeakerAckCount++];
          strlcpy(pending.requestId, speakerControlRequest.requestId, sizeof(pending.requestId));
          pending.targetEnabled = speakerControlRequest.enabled;
          pending.applied = true;
          pending.startedMs = millis();
          deferAck = true;
        } else {
          applied = false;
        }
      }
    } else if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::TEMPORARY_CONNECT) {
      applied = newoSpeaker.requestTemporaryConnection();
      deferAck = true;  // The uncorrelated manual-test request needs no /device acknowledgement.
    }
    if (applied && speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_VOLUME) {
      newoLed.flashVolume(newoSpeaker.volume());
    } else if (applied && speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::TOGGLE_MUTE) {
      newoLed.flashMute(newoSpeaker.muted());
    } else if (applied && speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_ENABLED) {
      newoLed.flashSpeakerEnabled(newoSpeaker.enabled());
    }
    if (!deferAck) {
      newoCloud.sendSpeakerAck(speakerControlRequest.requestId, newoSpeaker.enabled(),
                               newoSpeaker.connectionStatus(), newoSpeaker.volume(), newoSpeaker.muted(),
                               applied, newoSpeaker.lastPlayback(), newoSpeaker.lastUnderruns(),
                               newoSpeaker.lastOverflows());
    }
  }
  newoAudio.loop();
  newoSpeaker.loop(newoCloud.connected());
  for (uint8_t i = 0; i < pendingSpeakerAckCount;) {
    SpeakerAck& pending = pendingSpeakerAcks[i];
    const bool complete = pending.targetEnabled ? newoSpeaker.ready() : newoSpeaker.released();
    const bool timedOut = millis() - pending.startedMs >= 6'500;
    if (!complete && !timedOut) { ++i; continue; }
    newoCloud.sendSpeakerAck(pending.requestId, newoSpeaker.enabled(), newoSpeaker.connectionStatus(),
                             newoSpeaker.volume(), newoSpeaker.muted(), pending.applied && complete,
                             newoSpeaker.lastPlayback(), newoSpeaker.lastUnderruns(),
                             newoSpeaker.lastOverflows());
    pendingSpeakerAcks[i] = pendingSpeakerAcks[--pendingSpeakerAckCount];
  }
  while (newoSpeaker.consumePlaybackStarted(speakerStarted)) {
    newoCloud.sendSpeakerStarted(speakerStarted.playbackId, speakerStarted.firstPcmToPlayMs);
  }
  while (newoSpeaker.consumeResult(speakerResult)) {
    newoCloud.sendSpeakerResult(speakerResult.playbackId, speakerResult.success,
                                speakerResult.bytes, speakerResult.error);
  }
  newoCloud.updateVoiceTelemetry(newoAudio.state(), newoAudio.voiceConnected(), newoAudio.wakeCount(),
                                  newoAudio.sessionCount(), newoAudio.failures(), newoAudio.timeouts());
  if (!newoAudio.transitionPending()) {
    for (uint8_t i = 0; i < pendingVoiceAckCount; ++i) {
      newoCloud.sendVoiceAck(pendingVoiceAcks[i].requestId, newoAudio.state(), newoAudio.voiceConnected(),
                             newoAudio.wakeCount(), newoAudio.sessionCount(), newoAudio.failures(),
                             newoAudio.timeouts(), pendingVoiceAcks[i].applied);
    }
    pendingVoiceAckCount = 0;
  }
  if (newoSpeaker.playing()) newoLed.setState(NewoLed::State::SPEAKING);
  else if (newoAudio.state() == NewoVoiceState::STREAMING) newoLed.setState(NewoLed::State::LISTENING);
  else if (newoCloud.assistantThinking()) newoLed.setState(NewoLed::State::THINKING);
  else newoLed.setState(NewoLed::State::IDLE);
  newoLed.loop();
  newoDisplay.updateTelemetry(newoWiFi.connected(), newoWiFi.rssi(), newoCloud.connected(), millis(),
                              ESP.getFreeHeap(), ESP.getFreePsram(), NewoLog::stats());
  newoDisplay.loop();
  newoDisplay.updateClock();
  delay(2);
}
