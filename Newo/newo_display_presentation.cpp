#include "newo_display.h"

namespace {
const char* effectName(NewoSecondaryEffect effect) {
  switch (effect) {
    case NewoSecondaryEffect::ZZZ: return "ZZZ";
    case NewoSecondaryEffect::QUESTION: return "QUESTION";
    case NewoSecondaryEffect::EXCLAMATION: return "EXCLAMATION";
    case NewoSecondaryEffect::SURPRISE_MARK: return "SURPRISE_MARK";
    case NewoSecondaryEffect::ELLIPSIS: return "ELLIPSIS";
    case NewoSecondaryEffect::SWEAT: return "SWEAT";
    case NewoSecondaryEffect::NONE: return "NONE";
  }
  return "UNKNOWN";
}

const char* captionName(NewoFaceCaption caption) {
  switch (caption) {
    case NewoFaceCaption::HUH: return "HUH";
    case NewoFaceCaption::WOAH: return "WOAH";
    case NewoFaceCaption::HMM: return "HMM";
    case NewoFaceCaption::HEY: return "HEY";
    case NewoFaceCaption::WTF: return "WTF";
    case NewoFaceCaption::TSK: return "TSK";
    case NewoFaceCaption::NONE: return "NONE";
  }
  return "UNKNOWN";
}
}  // namespace

void NewoDisplay::clearAutonomousPresentation() {
  autonomousPresentation_ = {};
  autonomousPresentationStartedMs_ = 0;
  autonomousEffectUntilMs_ = 0;
  autonomousCaptionUntilMs_ = 0;
}

void NewoDisplay::requestAutonomousPresentation(NewoPresentationCue cue, uint8_t intensity,
                                                 uint32_t now) {
  autonomousPresentation_ = newoComposePresentation(
      cue, intensity, static_cast<uint8_t>(random(0, 256)));
  autonomousPresentationStartedMs_ = now;
  autonomousEffectUntilMs_ = autonomousPresentation_.effect != NewoSecondaryEffect::NONE &&
                                     autonomousPresentation_.effectDurationMs != 0
                                 ? now + autonomousPresentation_.effectDurationMs
                                 : 0;
  autonomousCaptionUntilMs_ = autonomousPresentation_.caption != NewoFaceCaption::NONE &&
                                      autonomousPresentation_.captionDurationMs != 0
                                  ? now + autonomousPresentation_.captionDurationMs
                                  : 0;
  nextFaceFrameMs_ = 0;

  if (autonomousPresentation_.effect != NewoSecondaryEffect::NONE ||
      autonomousPresentation_.caption != NewoFaceCaption::NONE) {
    Serial.printf("[EYES] presentation effect=%s caption=%s\n",
                  effectName(autonomousPresentation_.effect),
                  captionName(autonomousPresentation_.caption));
  }
}