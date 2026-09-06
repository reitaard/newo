#include <Arduino.h>

#include "newo_audio.h"
#include "newo_arduino_node.h"
#include "newo_cloud.h"
#include "newo_config.h"
#include "newo_display.h"
#include "newo_log.h"
#include "newo_led.h"
#include "newo_physical_voice.h"
#include "newo_speaker.h"
#include "newo_storage.h"
#include "newo_usb_audio.h"
#include "newo_usb_host.h"
#include "newo_usb_storage.h"
#include "newo_usb_vcp.h"
#include "newo_wifi.h"

NewoStorage newoStorage;
NewoUsbStorage newoUsbStorage;
NewoWiFi newoWiFi(newoStorage);
NewoLed newoLed;
NewoDisplay newoDisplay;
NewoCloud newoCloud(newoWiFi, newoDisplay, newoStorage);
NewoAudio newoAudio(newoWiFi, newoDisplay);
NewoSpeaker newoSpeaker(newoWiFi, newoDisplay, newoAudio, newoStorage);

namespace {
using NewoPhysicalVoice::LedState;

LedState nanoLedDesired = LedState::IDLE;
LedState nanoLedSent = LedState::IDLE;
bool nanoLedSentValid = false;
uint32_t nanoLedHandshake = 0;
uint32_t nanoLedRetryAfterMs = 0;
uint32_t nanoLedErrorUntilMs = 0;
NewoPhysicalVoice::TriggerGate physicalTriggerGate;

void serviceNanoLed() {
  if (!newoArduinoNode.ready()) return;
  const uint32_t handshake = newoArduinoNode.handshakeGeneration();
  if (handshake != nanoLedHandshake) {
    nanoLedHandshake = handshake;
    nanoLedSentValid = false;
  }
  if ((nanoLedSentValid && nanoLedSent == nanoLedDesired) ||
      static_cast<int32_t>(millis() - nanoLedRetryAfterMs) < 0) return;
  if (newoArduinoNode.request("led", NewoPhysicalVoice::ledPayload(nanoLedDesired)) != 0) {
    nanoLedSent = nanoLedDesired;
    nanoLedSentValid = true;
    Serial.printf("[arduino] LED_STATE %s\n", NewoPhysicalVoice::ledName(nanoLedDesired));
  } else {
    nanoLedRetryAfterMs = millis() + 250;
  }
}
}  // namespace

void printHardwareInfo() {
  Serial.println();
  Serial.println("================================");
  Serial.println("            NEWO");
  Serial.println("================================");
  Serial.printf("Firmware: %s\n", NewoConfig::FIRMWARE_VERSION);
  Serial.printf("Autonomy: V%u\n", static_cast<unsigned>(NewoConfig::AUTONOMY_REVISION));
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
    newoDisplay.setClockEnabled(newoStorage.clockEnabled());
    char detail[48];
    snprintf(detail, sizeof(detail), "saved_networks=%u", static_cast<unsigned>(newoStorage.count()));
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::STORAGE, "STORAGE_READY", detail);
  }

  newoWiFi.begin();
  newoCloud.begin();
  newoAudio.begin();
  newoSpeaker.begin();

  // Install one physical host first, register every independent client while
  // enumeration is still stopped, then start the shared event pump. A D07,
  // flash drive or Arduino already plugged into the hub at power-on therefore
  // cannot race past a client that has not registered yet.
  if (!newoUsbHost.begin()) {
    Serial.println("[usb-host] HOST_FAILED — reason=startup");
  } else {
    if (!newoUsbAudio.begin(newoUsbHost)) {
      Serial.println("[usb-uac2] CLIENT_FAILED — reason=startup");
    }
    if (!newoUsbStorage.begin(newoUsbHost)) {
      Serial.println("[usb-storage] CLIENT_FAILED — reason=startup");
    }
    if (!newoUsbVcp.begin(newoUsbHost)) {
      Serial.println("[usb-vcp] CLIENT_FAILED — reason=startup");
    } else if (!newoArduinoNode.begin(newoUsbVcp)) {
      Serial.println("[arduino] START_FAILED — reason=startup");
    }
    if (!newoUsbHost.start()) {
      Serial.println("[usb-host] HOST_FAILED — reason=event_pump");
    }
  }

  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::BOOT, "BOOT_READY");
  newoCloud.recordStack("after boot");
}

void loop() {
  struct VoiceAck { char requestId[40]; bool applied; };
  struct SpeakerAck { char requestId[40]; bool targetEnabled; bool applied; bool ledFeedback; uint32_t startedMs; };
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
  while (newoCloud.consumeVoiceRequest(voiceRequest)) {
    bool applied = true;
    if (voiceRequest.action == NewoCloud::VoiceRequest::Action::MANUAL_TOGGLE) {
      applied = newoAudio.manualToggle();
    } else {
      bool enable = voiceRequest.action == NewoCloud::VoiceRequest::Action::ON;
      if (voiceRequest.action == NewoCloud::VoiceRequest::Action::TOGGLE) {
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
  NewoArduinoNode::Event arduinoEvent;
  while (newoArduinoNode.receiveEvent(arduinoEvent)) {
    if (strcmp(arduinoEvent.name, "voice_trigger") != 0 || strcmp(arduinoEvent.payload, "reset") != 0) continue;
    Serial.println("[arduino] EVENT voice_trigger reset");
    const char* rejection = nullptr;
    const auto decision = physicalTriggerGate.decide(
        newoArduinoNode.handshakeGeneration(), newoAudio.state() == NewoVoiceState::STREAMING,
        newoSpeaker.playing());
    if (decision == NewoPhysicalVoice::TriggerDecision::DUPLICATE) rejection = "duplicate";
    else if (decision == NewoPhysicalVoice::TriggerDecision::VOICE_ACTIVE) rejection = "voice_active";
    else if (decision == NewoPhysicalVoice::TriggerDecision::SPEAKER_BUSY) rejection = "speaker_busy";
    if (rejection == nullptr && newoAudio.startPhysicalVoiceTrigger()) {
      Serial.println("[voice] PHYSICAL_TRIGGER_ACCEPTED");
      nanoLedErrorUntilMs = 0;
    } else {
      if (rejection == nullptr) rejection = "unavailable";
      Serial.printf("[voice] PHYSICAL_TRIGGER_REJECTED reason=%s\n", rejection);
      nanoLedErrorUntilMs = millis() + 800;
    }
  }
  while (newoCloud.consumeSpeakerControlRequest(speakerControlRequest)) {
    bool applied = true;
    bool deferAck = false;
    bool speakerStateConfirmed = false;
    if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_VOLUME) {
      applied = newoSpeaker.setVolume(speakerControlRequest.volume);
    } else if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::TOGGLE_MUTE) {
      applied = newoSpeaker.setMuted(!newoSpeaker.muted());
    } else if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_ENABLED) {
      applied = newoSpeaker.setEnabled(speakerControlRequest.enabled);
      const bool complete = speakerControlRequest.enabled ? newoSpeaker.ready() : newoSpeaker.released();
      speakerStateConfirmed = applied && complete;
      if (applied && !complete) {
        if (pendingSpeakerAckCount < sizeof(pendingSpeakerAcks) / sizeof(pendingSpeakerAcks[0])) {
          SpeakerAck& pending = pendingSpeakerAcks[pendingSpeakerAckCount++];
          strlcpy(pending.requestId, speakerControlRequest.requestId, sizeof(pending.requestId));
          pending.targetEnabled = speakerControlRequest.enabled;
          pending.applied = true;
          pending.ledFeedback = speakerControlRequest.ledFeedback;
          pending.startedMs = millis();
          deferAck = true;
        } else {
          applied = false;
        }
      }
      if (speakerControlRequest.ledFeedback) {
        if (speakerStateConfirmed) newoLed.flashSpeakerEnabled(speakerControlRequest.enabled);
        else if (!applied) newoLed.flashError();
      }
    } else if (speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::TEMPORARY_CONNECT) {
      applied = newoSpeaker.requestTemporaryConnection();
      deferAck = true;
    }
    if (applied && speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::SET_VOLUME) {
      newoLed.flashVolume(newoSpeaker.volume());
    } else if (applied && speakerControlRequest.action == NewoCloud::SpeakerControlRequest::Action::TOGGLE_MUTE) {
      newoLed.flashMute(newoSpeaker.muted());
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
    const bool confirmed = pending.applied && complete;
    if (pending.ledFeedback) {
      if (confirmed) newoLed.flashSpeakerEnabled(pending.targetEnabled);
      else newoLed.flashError();
    }
    newoCloud.sendSpeakerAck(pending.requestId, newoSpeaker.enabled(), newoSpeaker.connectionStatus(),
                             newoSpeaker.volume(), newoSpeaker.muted(), confirmed,
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
  NewoArduinoNode::Acknowledgement arduinoAck;
  while (newoArduinoNode.receiveAcknowledgement(arduinoAck)) {
    if (!arduinoAck.success)
      Serial.printf("[arduino] REQUEST_REJECTED id=%lu\n", static_cast<unsigned long>(arduinoAck.requestId));
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
  if (newoSpeaker.audiblePlaybackActive()) newoLed.setState(NewoLed::State::SPEAKING);
  else if (newoAudio.state() == NewoVoiceState::STREAMING) newoLed.setState(NewoLed::State::LISTENING);
  else if (newoCloud.assistantThinking()) newoLed.setState(NewoLed::State::THINKING);
  else newoLed.setState(NewoLed::State::IDLE);
  if (static_cast<int32_t>(nanoLedErrorUntilMs - millis()) > 0) nanoLedDesired = LedState::ERROR;
  else if (newoSpeaker.audiblePlaybackActive()) nanoLedDesired = LedState::SPEAKING;
  else if (newoAudio.state() == NewoVoiceState::STREAMING) nanoLedDesired = LedState::LISTENING;
  else if (newoCloud.assistantThinking()) nanoLedDesired = LedState::THINKING;
  else nanoLedDesired = LedState::IDLE;
  serviceNanoLed();
  newoLed.loop();
  newoDisplay.updateTelemetry(newoWiFi.connected(), newoWiFi.rssi(), newoCloud.connected(), millis(),
                              ESP.getFreeHeap(), ESP.getFreePsram(), NewoLog::stats());
  newoDisplay.loop();
  newoDisplay.updateClock();
  delay(2);
}
