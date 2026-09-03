import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("behavior explicitly composes reusable effects and captions without coupling presentation to pose", async () => {
  const [display, header, presentationRuntime, presentationPolicy, pose] = await Promise.all([
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_display_presentation.cpp"),
    source("../../Newo/newo_presentation.cpp"),
    source("../../Newo/newo_display_pose.cpp"),
  ]);

  assert.match(header, /void requestAutonomousPresentation\(NewoPresentationCue cue, uint8_t intensity, uint32_t now\)/);
  assert.match(header, /void clearAutonomousPresentation\(\)/);
  assert.match(header, /NewoPresentationIntent autonomousPresentation_/);
  assert.match(header, /autonomousPresentationStartedMs_/);
  assert.match(header, /autonomousEffectUntilMs_/);
  assert.match(header, /autonomousCaptionUntilMs_/);

  // Entropy belongs to behavior/runtime; the pure policy remains Arduino-free.
  assert.match(presentationRuntime, /newoComposePresentation\([\s\S]*random\(0, 256\)/);
  assert.match(presentationRuntime, /\[EYES\] presentation effect=%s caption=%s/);
  assert.doesNotMatch(presentationPolicy, /random\(|millis\(|Arduino/);

  // Behavior episodes explicitly request composition. Expression itself never
  // permanently implies a symbol or caption.
  assert.match(display, /AutonomousEpisode::CURIOUS_SCAN[\s\S]*requestAutonomousPresentation\(NewoPresentationCue::CURIOUS/);
  assert.match(display, /AutonomousEpisode::LOW_ENERGY[\s\S]*requestAutonomousPresentation\(NewoPresentationCue::LOW_ENERGY/);
  assert.match(display, /AutonomousEpisode::SOCIAL_ATTENTION[\s\S]*requestAutonomousPresentation\(NewoPresentationCue::SOCIAL/);
  assert.match(display, /AutonomousEpisode::ALERT_CHECK[\s\S]*NewoPresentationCue::ALERT_SURPRISE[\s\S]*NewoPresentationCue::ALERT_CONFUSED/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::SLEEPING, 100\)[\s\S]*requestAutonomousPresentation\(NewoPresentationCue::SLEEPING, 100, now\)/);
  assert.doesNotMatch(display.match(/void NewoDisplay::setAutonomousExpression\([\s\S]*?\n\}/)?.[0] ?? "",
                      /NewoPresentationCue|requestAutonomousPresentation|NewoSecondaryEffect|NewoFaceCaption/);

  // Autonomous presentation cannot leak across episode/context boundaries.
  assert.match(display, /void NewoDisplay::resetAutonomousEpisode\([\s\S]*clearAutonomousPresentation\(\)/);
  assert.match(display, /void NewoDisplay::finishAutonomousEpisode\([\s\S]*clearAutonomousPresentation\(\)/);
  assert.match(display, /AutonomousEpisode::DROWSY_REST[\s\S]*autonomousEpisodeStep_ == 2[\s\S]*clearAutonomousPresentation\(\)[\s\S]*AutonomousExpression::SLEEPY/);

  // Manual Telegram test overrides remain first priority; auto composition is
  // consulted only after them and only while autonomous IDLE owns the face.
  const effectResolver = pose.match(/NewoSecondaryEffect NewoDisplay::secondaryEffectFor\([\s\S]*?\n\}/)?.[0] ?? "";
  assert.match(effectResolver, /mode != NewoDisplayMode::IDLE/);
  assert.match(effectResolver, /secondaryEffectOverride_[\s\S]*return secondaryEffectOverride_[\s\S]*autonomousPresentation_\.effect/);
  assert.match(effectResolver, /autonomousEffectUntilMs_/);

  const captionResolver = pose.match(/NewoFaceCaption NewoDisplay::faceCaptionFor\([\s\S]*?\n\}/)?.[0] ?? "";
  assert.match(captionResolver, /mode != NewoDisplayMode::IDLE/);
  assert.match(captionResolver, /faceCaptionOverride_[\s\S]*return faceCaptionOverride_[\s\S]*autonomousPresentation_\.caption/);
  assert.match(captionResolver, /autonomousCaptionUntilMs_/);

  // Presentation resolution may consume an already-composed intent, but it may
  // not reach back into behavior episodes or invoke the composer itself.
  assert.doesNotMatch(pose, /autonomousEpisode_/);
  assert.doesNotMatch(pose, /newoComposePresentation/);
  assert.match(pose, /manualEffectActive[\s\S]*autonomousEffectActive[\s\S]*effectStartedMs/);
  assert.match(pose, /manualCaptionActive[\s\S]*autonomousCaptionActive[\s\S]*captionStartedMs/);
});
