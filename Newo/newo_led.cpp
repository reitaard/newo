#include "newo_led.h"

#include "newo_config.h"

namespace {
constexpr uint32_t kFrameMs = 25;
constexpr uint32_t kConnectivityGraceMs = 3000;
constexpr uint8_t kRed = 7;
constexpr uint8_t kGreen = 7;
constexpr uint8_t kSpeakingGreen = 5;
// Blue is perceptually weaker on this confirmed onboard LED.
constexpr uint8_t kBlue = 10;
constexpr uint8_t kAmberRed = 9;
constexpr uint8_t kAmberGreen = 5;
constexpr uint8_t kWhite = 8;

bool inPulse(uint32_t elapsed, uint32_t start, uint32_t length) {
  return elapsed >= start && elapsed < start + length;
}
}  // namespace

void NewoLed::begin() {
  // Newo intentionally starts physically dark.
  rgbLedWrite(NewoConfig::RGB_LED_PIN, 0, 0, 0);
  last_ = {0, 0, 0};
  lastWriteMs_ = millis();
}

void NewoLed::setConnectivity(bool available) {
  const uint32_t now = millis();
  if (available == connectivityAvailable_) return;
  connectivityAvailable_ = available;
  if (!available) {
    connectivityLostAtMs_ = now;
    connectivityWasLost_ = false;
  } else if (connectivityWasLost_) {
    flashPing();
    connectivityWasLost_ = false;
  }
}

void NewoLed::startOverlay(Overlay overlay, uint8_t value) {
  overlay_ = overlay;
  overlayStartedMs_ = millis();
  overlayValue_ = value;
}
void NewoLed::flashError() { startOverlay(Overlay::ERROR); }
void NewoLed::flashPing() { startOverlay(Overlay::PING); }
void NewoLed::flashSpeakerEnabled(bool enabled) { startOverlay(enabled ? Overlay::SPEAKER_ON : Overlay::SPEAKER_OFF); }
void NewoLed::flashMute(bool muted) { startOverlay(muted ? Overlay::MUTE_ON : Overlay::MUTE_OFF); }
void NewoLed::flashVolume(uint8_t volume) { startOverlay(Overlay::VOLUME, volume); }
void NewoLed::startRebootSequence() { startOverlay(Overlay::REBOOT); }
void NewoLed::flashProvisioningAccepted() { startOverlay(Overlay::PROV_ACCEPTED); }
void NewoLed::flashProvisioningRejected() { startOverlay(Overlay::PROV_REJECTED); }
void NewoLed::flashProvisioningSaved() { startOverlay(Overlay::PROV_SAVED); }
void NewoLed::flashProvisioningTimeout() { startOverlay(Overlay::PROV_TIMEOUT); }

uint8_t NewoLed::pulse(uint32_t elapsed, uint32_t duration, uint8_t peak) {
  if (elapsed >= duration) return 0;
  const uint32_t half = duration / 2;
  const uint32_t distance = elapsed < half ? elapsed : duration - elapsed;
  return static_cast<uint8_t>((distance * peak) / (half ? half : 1));
}

NewoLed::Rgb NewoLed::renderBase(uint32_t now) {
  if (state_ == State::SPEAKING) return {0, kSpeakingGreen, 0};
  if (state_ == State::LISTENING) {
    // Asymmetric gentle pulse; capture/task timing remains untouched.
    const uint32_t phase = now % 1200;
    const uint8_t level = phase < 180 ? static_cast<uint8_t>(kBlue + (phase * 5) / 180)
                                      : phase < 760 ? 15 : static_cast<uint8_t>(15 - ((phase - 760) * 5) / 440);
    return {0, 0, level};
  }
  if (state_ == State::THINKING) {
    const uint32_t phase = now % 1000;
    if (phase < 100) return {5, 0, 7};       // strong heartbeat
    if (phase >= 200 && phase < 280) return {3, 0, 4};  // weaker heartbeat
    return {0, 0, 0};
  }
  if (provisioning_) {
    const uint32_t phase = now % 1800;
    if (inPulse(phase, 0, 100) || inPulse(phase, 220, 80)) return {0, 5, 7};
    return {0, 0, 0};
  }
  if (!connectivityAvailable_ && connectivityLostAtMs_ != 0 && now - connectivityLostAtMs_ >= kConnectivityGraceMs) {
    if (now - connectivityLostAtMs_ >= kConnectivityGraceMs) connectivityWasLost_ = true;
    const uint32_t phase = (now - connectivityLostAtMs_ - kConnectivityGraceMs) % 2200;
    if (inPulse(phase, 0, 90) || inPulse(phase, 170, 70)) return {kAmberRed, kAmberGreen, 0};
  }
  return {0, 0, 0};
}

bool NewoLed::renderOverlay(uint32_t elapsed, Rgb& color) const {
  switch (overlay_) {
    case Overlay::PING:
      if (elapsed >= 250) return false;
      { const uint8_t v = static_cast<uint8_t>((250 - elapsed) * kWhite / 250); color = {v, v, v}; return true; }
    case Overlay::ERROR:
      if (elapsed >= 780) return false;
      if (inPulse(elapsed, 0, 110) || inPulse(elapsed, 180, 110) || inPulse(elapsed, 360, 110)) color = {kRed, 0, 0};
      else color = {0, 0, 0};
      return true;
    case Overlay::SPEAKER_ON:
    case Overlay::SPEAKER_OFF:
      if (elapsed >= 300) return false;
      if (inPulse(elapsed, 0, 70) || inPulse(elapsed, 140, 100)) {
        const bool on = overlay_ == Overlay::SPEAKER_ON;
        const uint8_t v = on ? (elapsed < 100 ? 4 : kGreen) : (elapsed < 100 ? kRed : 4);
        color = on ? Rgb{0, v, 0} : Rgb{v, 0, 0};
      } else color = {0, 0, 0};
      return true;
    case Overlay::MUTE_ON: if (elapsed >= 160) return false; color = {kAmberRed, 2, 0}; return true;
    case Overlay::MUTE_OFF: if (elapsed >= 160) return false; color = {0, kGreen, 0}; return true;
    case Overlay::VOLUME:
      if (elapsed >= 160) return false;
      color = {0, static_cast<uint8_t>(2 + (overlayValue_ * (kGreen - 2)) / 100), 0}; return true;
    case Overlay::REBOOT: {
      // One deliberate non-blocking green breath inside the existing reboot delay.
      constexpr uint32_t kRebootBreathMs = 900;
      constexpr uint8_t kRebootMinGreen = 2;
      constexpr uint8_t kRebootPeakGreen = 12;
      if (elapsed >= kRebootBreathMs) return false;
      const uint32_t half = kRebootBreathMs / 2;
      const uint32_t distance = elapsed < half ? elapsed : kRebootBreathMs - elapsed;
      const uint8_t green = static_cast<uint8_t>(kRebootMinGreen +
          (distance * (kRebootPeakGreen - kRebootMinGreen)) / half);
      color = {0, green, 0};
      return true;
    }
    case Overlay::PROV_ACCEPTED:
    case Overlay::PROV_SAVED:
      if (elapsed >= 300) return false;
      color = (inPulse(elapsed, 0, 90) || inPulse(elapsed, 160, 90)) ? Rgb{0, kGreen, 0} : Rgb{0, 0, 0}; return true;
    case Overlay::PROV_REJECTED:
      if (elapsed >= 320) return false;
      color = (inPulse(elapsed, 0, 70) || inPulse(elapsed, 120, 70) || inPulse(elapsed, 240, 70)) ? Rgb{kRed, 0, 0} : Rgb{0, 0, 0}; return true;
    case Overlay::PROV_TIMEOUT:
      if (elapsed >= 500) return false;
      color = {pulse(elapsed, 500, kAmberRed), pulse(elapsed, 500, kAmberGreen), 0}; return true;
    case Overlay::NONE: return false;
  }
  return false;
}

void NewoLed::write(Rgb color, uint32_t now) {
  if (color.r == last_.r && color.g == last_.g && color.b == last_.b) return;
  if (now - lastWriteMs_ < kFrameMs) return;
  rgbLedWrite(NewoConfig::RGB_LED_PIN, color.r, color.g, color.b);
  last_ = color;
  lastWriteMs_ = now;
}

void NewoLed::loop() {
  const uint32_t now = millis();
  Rgb color = renderBase(now);
  if (overlay_ != Overlay::NONE && !renderOverlay(now - overlayStartedMs_, color)) overlay_ = Overlay::NONE;
  write(color, now);
}
