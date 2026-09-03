#pragma once

#include <stdint.h>

enum class NewoSecondaryEffect : uint8_t {
  NONE,
  ZZZ,
  QUESTION,
  EXCLAMATION,
  SURPRISE_MARK,
  ELLIPSIS,
  SWEAT,
};

enum class NewoFaceCaption : uint8_t {
  NONE,
  HUH,
  WOAH,
  HMM,
  HEY,
};

// Semantic request from behavior to the presentation layer. These cues are not
// eye poses and they do not draw anything themselves.
enum class NewoPresentationCue : uint8_t {
  NONE,
  CURIOUS,
  SOCIAL,
  ALERT_SURPRISE,
  ALERT_CONFUSED,
  LOW_ENERGY,
  SLEEPING,
};

struct NewoPresentationIntent {
  NewoSecondaryEffect effect = NewoSecondaryEffect::NONE;
  NewoFaceCaption caption = NewoFaceCaption::NONE;
  uint16_t effectDurationMs = 0;
  uint16_t captionDurationMs = 0;
};

// Pure deterministic composition policy. `variation` is caller-supplied
// entropy in 0..255 so behavior can remain random without making this engine
// depend on Arduino random(), timing, display state, or heap allocation.
NewoPresentationIntent newoComposePresentation(NewoPresentationCue cue, uint8_t intensity,
                                               uint8_t variation);
