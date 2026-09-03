#include <cstdlib>
#include <iostream>

#include "../Newo/newo_presentation.h"

namespace {
void require(bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void testCuriousComposition() {
  auto intent = newoComposePresentation(NewoPresentationCue::CURIOUS, 40, 0);
  require(intent.effect == NewoSecondaryEffect::NONE && intent.caption == NewoFaceCaption::NONE,
          "weak curiosity should remain undecorated");

  intent = newoComposePresentation(NewoPresentationCue::CURIOUS, 85, 4);
  require(intent.effect == NewoSecondaryEffect::QUESTION && intent.caption == NewoFaceCaption::HUH,
          "strong curious reaction should be able to combine question + Huh");

  intent = newoComposePresentation(NewoPresentationCue::CURIOUS, 70, 25);
  require(intent.effect == NewoSecondaryEffect::QUESTION && intent.caption == NewoFaceCaption::NONE,
          "curiosity should also support symbol-only composition");

  intent = newoComposePresentation(NewoPresentationCue::CURIOUS, 90, 44);
  require(intent.effect == NewoSecondaryEffect::NONE && intent.caption == NewoFaceCaption::HUH,
          "strong curiosity should support caption-only variation");
}

void testAlertComposition() {
  auto intent = newoComposePresentation(NewoPresentationCue::ALERT_SURPRISE, 70, 50);
  require(intent.effect == NewoSecondaryEffect::EXCLAMATION,
          "moderate surprise should request exclamation");
  require(intent.caption == NewoFaceCaption::NONE,
          "surprise caption should remain optional");

  intent = newoComposePresentation(NewoPresentationCue::ALERT_SURPRISE, 90, 10);
  require(intent.effect == NewoSecondaryEffect::SURPRISE_MARK && intent.caption == NewoFaceCaption::WOAH,
          "strong surprise should be able to combine mark + Woah");

  intent = newoComposePresentation(NewoPresentationCue::ALERT_CONFUSED, 80, 12);
  require(intent.effect == NewoSecondaryEffect::QUESTION && intent.caption == NewoFaceCaption::HMM,
          "confusion should be able to combine question + Hmm");

  intent = newoComposePresentation(NewoPresentationCue::ALERT_CONFUSED, 80, 62);
  require(intent.effect == NewoSecondaryEffect::ELLIPSIS && intent.caption == NewoFaceCaption::NONE,
          "confusion should have a quieter ellipsis variation");
}

void testSocialFatigueAndSleep() {
  auto intent = newoComposePresentation(NewoPresentationCue::SOCIAL, 75, 8);
  require(intent.effect == NewoSecondaryEffect::NONE && intent.caption == NewoFaceCaption::HEY,
          "social attention should occasionally use Hey without a symbol");

  intent = newoComposePresentation(NewoPresentationCue::LOW_ENERGY, 65, 5);
  require(intent.effect == NewoSecondaryEffect::NONE,
          "moderate low-energy intent should not overdecorate");

  intent = newoComposePresentation(NewoPresentationCue::LOW_ENERGY, 85, 5);
  require(intent.effect == NewoSecondaryEffect::ELLIPSIS,
          "strong low-energy intent should occasionally request ellipsis");

  intent = newoComposePresentation(NewoPresentationCue::SLEEPING, 100, 99);
  require(intent.effect == NewoSecondaryEffect::ZZZ && intent.caption == NewoFaceCaption::NONE,
          "sleep presentation should consistently request ZZZ without speech-like text");
}

void testBoundsAndDeterminism() {
  const auto first = newoComposePresentation(NewoPresentationCue::CURIOUS, 255, 104);
  const auto second = newoComposePresentation(NewoPresentationCue::CURIOUS, 100, 4);
  require(first.effect == second.effect && first.caption == second.caption &&
              first.effectDurationMs == second.effectDurationMs &&
              first.captionDurationMs == second.captionDurationMs,
          "intensity and variation inputs should normalize deterministically");

  for (uint8_t cue = 0; cue <= static_cast<uint8_t>(NewoPresentationCue::SLEEPING); ++cue) {
    for (uint16_t variation = 0; variation < 100; ++variation) {
      const auto intent = newoComposePresentation(static_cast<NewoPresentationCue>(cue), 100,
                                                  static_cast<uint8_t>(variation));
      require(intent.effectDurationMs <= 15'000, "effect duration must stay inside firmware bound");
      require(intent.captionDurationMs <= 8'000, "caption duration must stay inside firmware bound");
    }
  }
}
}  // namespace

int main() {
  testCuriousComposition();
  testAlertComposition();
  testSocialFatigueAndSleep();
  testBoundsAndDeterminism();
  std::cout << "presentation-host-test: PASS\n";
  return 0;
}
