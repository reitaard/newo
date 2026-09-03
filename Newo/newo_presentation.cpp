#include "newo_presentation.h"

namespace {
uint8_t clampPercent(uint8_t value) {
  return value > 100 ? 100 : value;
}

uint8_t normalizeVariation(uint8_t value) {
  return static_cast<uint8_t>(value % 100);
}
}  // namespace

NewoPresentationIntent newoComposePresentation(NewoPresentationCue cue, uint8_t intensity,
                                               uint8_t variation) {
  NewoPresentationIntent intent;
  const uint8_t strength = clampPercent(intensity);
  const uint8_t roll = normalizeVariation(variation);

  switch (cue) {
    case NewoPresentationCue::CURIOUS:
      if (strength < 55) return intent;
      if (roll < 16) {
        intent.effect = NewoSecondaryEffect::QUESTION;
        intent.caption = NewoFaceCaption::HUH;
        intent.effectDurationMs = 2'200;
        intent.captionDurationMs = 1'800;
      } else if (roll < 40) {
        intent.effect = NewoSecondaryEffect::QUESTION;
        intent.effectDurationMs = 2'200;
      } else if (strength >= 80 && roll < 48) {
        intent.caption = NewoFaceCaption::HUH;
        intent.captionDurationMs = 1'800;
      }
      return intent;

    case NewoPresentationCue::SOCIAL:
      if (strength >= 60 && roll < 18) {
        intent.caption = NewoFaceCaption::HEY;
        intent.captionDurationMs = 1'600;
      }
      return intent;

    case NewoPresentationCue::ALERT_SURPRISE:
      intent.effect = strength >= 80 ? NewoSecondaryEffect::SURPRISE_MARK
                                     : NewoSecondaryEffect::EXCLAMATION;
      intent.effectDurationMs = 1'900;
      if (roll < 28) {
        intent.caption = NewoFaceCaption::WOAH;
        intent.captionDurationMs = 1'700;
      }
      return intent;

    case NewoPresentationCue::ALERT_CONFUSED:
      if (roll < 55) {
        intent.effect = NewoSecondaryEffect::QUESTION;
        intent.effectDurationMs = 2'200;
      } else if (roll < 72) {
        intent.effect = NewoSecondaryEffect::ELLIPSIS;
        intent.effectDurationMs = 2'400;
      }
      if (roll < 24) {
        intent.caption = NewoFaceCaption::HMM;
        intent.captionDurationMs = 1'900;
      }
      return intent;

    case NewoPresentationCue::LOW_ENERGY:
      if (strength >= 75 && roll < 30) {
        intent.effect = NewoSecondaryEffect::ELLIPSIS;
        intent.effectDurationMs = 2'400;
      }
      return intent;

    case NewoPresentationCue::SLEEPING:
      intent.effect = NewoSecondaryEffect::ZZZ;
      intent.effectDurationMs = 7'000;
      return intent;

    case NewoPresentationCue::NONE:
      return intent;
  }

  return intent;
}
