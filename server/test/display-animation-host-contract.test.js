import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("pure display engines remain host portable and behavior-tested", async () => {
  const [poseHeader, gazeHeader, hostTest, runner] = await Promise.all([
    source("../../Newo/newo_eye_pose.h"),
    source("../../Newo/newo_gaze_motion.h"),
    source("../../tools/display-animation-host-test.cpp"),
    source("../../tools/run-display-animation-host-test.sh"),
  ]);

  assert.match(poseHeader, /#include <stdint\.h>/);
  assert.match(gazeHeader, /#include <stdint\.h>/);
  assert.doesNotMatch(poseHeader, /Arduino\.h/);
  assert.doesNotMatch(gazeHeader, /Arduino\.h/);

  assert.match(hostTest, /testPoseTransitionDoesNotRestart/);
  assert.match(hostTest, /testDirectionAwareClosureHandoff/);
  assert.match(hostTest, /testGazeEnvelopeAndCompletion/);
  assert.match(hostTest, /destinationX < -kHardX/);
  assert.match(hostTest, /require\(!motion\.active\(\)/);
  assert.match(hostTest, /display-animation-host-test: PASS/);

  assert.match(runner, /-std=c\+\+17/);
  assert.match(runner, /-Wall -Wextra -Werror/);
  assert.match(runner, /newo_eye_pose\.cpp/);
  assert.match(runner, /newo_gaze_motion\.cpp/);
});
