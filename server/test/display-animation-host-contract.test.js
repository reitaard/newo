import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("pure display engines remain host portable and behavior-tested", async () => {
  const [poseHeader, gazeHeader, stateHeader, presentationHeader, hostTest, stateHostTest, presentationHostTest, runner] = await Promise.all([
    source("../../Newo/newo_eye_pose.h"),
    source("../../Newo/newo_gaze_motion.h"),
    source("../../Newo/newo_autonomy_state.h"),
    source("../../Newo/newo_presentation.h"),
    source("../../tools/display-animation-host-test.cpp"),
    source("../../tools/autonomy-state-host-test.cpp"),
    source("../../tools/presentation-host-test.cpp"),
    source("../../tools/run-display-animation-host-test.sh"),
  ]);

  for (const header of [poseHeader, gazeHeader, stateHeader, presentationHeader]) {
    assert.match(header, /#include <stdint\.h>/);
    assert.doesNotMatch(header, /Arduino\.h/);
  }

  assert.match(hostTest, /testPoseTransitionDoesNotRestart/);
  assert.match(hostTest, /testDirectionAwareClosureHandoff/);
  assert.match(hostTest, /testGazeEnvelopeAndCompletion/);
  assert.match(hostTest, /destinationX < -kHardX/);
  assert.match(hostTest, /require\(!motion\.active\(\)/);
  assert.match(hostTest, /display-animation-host-test: PASS/);

  assert.match(stateHeader, /class NewoAutonomyState/);
  assert.match(stateHeader, /fatigue\(\) const/);
  assert.match(stateHeader, /NewoInactivityStage stage\(uint32_t now\) const/);
  assert.doesNotMatch(stateHeader, /malloc|new\s|std::vector|std::map/);
  assert.match(stateHostTest, /testResetAndStages/);
  assert.match(stateHostTest, /testIdleEnergyLifecycleAndFloor/);
  assert.match(stateHostTest, /testInteractionRecoveryAndBounds/);
  assert.match(stateHostTest, /testStressAndCuriosityDecay/);
  assert.match(stateHostTest, /autonomy-state-host-test: PASS/);

  assert.match(presentationHeader, /struct NewoPresentationIntent/);
  assert.match(presentationHeader, /enum class NewoPresentationCue/);
  assert.match(presentationHeader, /newoComposePresentation/);
  assert.doesNotMatch(presentationHeader, /malloc|new\s|std::vector|std::map/);
  assert.match(presentationHostTest, /testCuriousComposition/);
  assert.match(presentationHostTest, /testAlertComposition/);
  assert.match(presentationHostTest, /testSocialFatigueSleepAndUnimpressed/);
  assert.match(presentationHostTest, /testBoundsAndDeterminism/);
  assert.match(presentationHostTest, /presentation-host-test: PASS/);

  assert.match(runner, /-std=c\+\+17/);
  assert.match(runner, /-Wall -Wextra -Werror/);
  assert.match(runner, /newo_eye_pose\.cpp/);
  assert.match(runner, /newo_gaze_motion\.cpp/);
  assert.match(runner, /newo_autonomy_state\.cpp/);
  assert.match(runner, /newo_presentation\.cpp/);
  assert.match(runner, /autonomy-state-host-test\.cpp/);
  assert.match(runner, /presentation-host-test\.cpp/);
});
