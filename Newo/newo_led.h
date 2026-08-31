#pragma once

#include <Arduino.h>

// GPIO48's sole production owner. Inputs are state/events; rendering occurs only
// from loop() on the Arduino loop task.
class NewoLed {
 public:
  enum class State : uint8_t { IDLE, LISTENING, THINKING, SPEAKING };

  void begin();
  void loop();
  void setState(State state) { state_ = state; }
  void setConnectivity(bool available);
  void setProvisioning(bool active) { provisioning_ = active; }
  void flashError();
  void flashPing();
  void flashSpeakerEnabled(bool enabled);
  void flashMute(bool muted);
  void flashVolume(uint8_t volume);
  void startRebootSequence();
  void flashProvisioningAccepted();
  void flashProvisioningRejected();
  void flashProvisioningSaved();
  void flashProvisioningTimeout();

 private:
  enum class Overlay : uint8_t { NONE, ERROR, PING, SPEAKER_ON, SPEAKER_OFF, MUTE_ON, MUTE_OFF,
                                 VOLUME, REBOOT, PROV_ACCEPTED, PROV_REJECTED, PROV_SAVED, PROV_TIMEOUT };
  struct Rgb { uint8_t r, g, b; };
  void startOverlay(Overlay overlay, uint8_t value = 0);
  Rgb renderBase(uint32_t now);
  bool renderOverlay(uint32_t elapsed, Rgb& color) const;
  void write(Rgb color, uint32_t now);
  static uint8_t pulse(uint32_t elapsed, uint32_t duration, uint8_t peak);

  State state_ = State::IDLE;
  bool provisioning_ = false;
  bool connectivityAvailable_ = false;
  bool connectivityWasLost_ = false;
  uint32_t connectivityLostAtMs_ = 0;
  Overlay overlay_ = Overlay::NONE;
  uint32_t overlayStartedMs_ = 0;
  uint8_t overlayValue_ = 0;
  Rgb last_ = {255, 255, 255};
  uint32_t lastWriteMs_ = 0;
};
