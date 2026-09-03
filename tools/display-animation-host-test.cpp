#include <cstdlib>
#include <iostream>

#include "../Newo/newo_eye_pose.h"
#include "../Newo/newo_gaze_motion.h"

namespace {
void require(bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void testPoseTransitionDoesNotRestart() {
  NewoEyePose neutral;
  NewoEyePose curious = neutral;
  curious.leftWidth = 50;
  curious.leftHeight = 30;
  curious.rightWidth = 68;
  curious.rightHeight = 50;

  NewoEyePoseEngine engine;
  engine.reset(neutral, 0);
  for (uint32_t now = 0; now <= 400; now += 50) {
    engine.transitionTo(curious, now, 400, NewoEyeEasing::EASE_IN_OUT);
    engine.update(now);
  }

  const NewoEyePose& finalPose = engine.update(400);
  require(!engine.active(), "repeated identical target requests must not restart the transition");
  require(finalPose.leftWidth == curious.leftWidth, "pose transition must reach left width target");
  require(finalPose.rightHeight == curious.rightHeight, "pose transition must reach right height target");
}

void testDirectionAwareClosureHandoff() {
  NewoEyePose open;
  NewoEyePose closed = open;
  closed.leftHeight = closed.rightHeight = 12;
  closed.openness = 0;
  closed.closureStyle = NewoEyeClosureStyle::CURVED;

  NewoEyePoseEngine engine;
  engine.reset(open, 0);
  engine.transitionTo(closed, 0, 400, NewoEyeEasing::EASE_IN_OUT);
  require(engine.update(250).closureStyle == NewoEyeClosureStyle::FILLED,
          "closing must keep filled geometry until late in the morph");
  require(engine.update(300).closureStyle == NewoEyeClosureStyle::CURVED,
          "closing must switch to curved geometry near the end");

  engine.reset(closed, 0);
  engine.transitionTo(open, 0, 400, NewoEyeEasing::EASE_IN_OUT);
  require(engine.update(100).closureStyle == NewoEyeClosureStyle::CURVED,
          "waking must keep the closed curve during the first small opening");
  require(engine.update(150).closureStyle == NewoEyeClosureStyle::FILLED,
          "waking must return to filled geometry early enough to expand smoothly");
}

void runGazeCase(int16_t startX, int16_t startY, int16_t destinationX, int16_t destinationY,
                 bool expressive) {
  constexpr int16_t kHardX = 20;
  constexpr int16_t kHardY = 12;
  NewoGazeMotion motion;
  motion.reset(startX, startY);
  motion.start(startX, startY, destinationX, destinationY, kHardX, kHardY, expressive);

  int16_t x = startX;
  int16_t y = startY;
  bool done = false;
  for (int step = 0; step < 128; ++step) {
    done = motion.update(x, y);
    require(x >= -kHardX && x <= kHardX, "gaze X left approved envelope");
    require(y >= -kHardY && y <= kHardY, "gaze Y left approved envelope");
    if (done) break;
  }

  require(done, "gaze motion must terminate");
  const int16_t expectedX = destinationX < -kHardX ? -kHardX : destinationX > kHardX ? kHardX : destinationX;
  const int16_t expectedY = destinationY < -kHardY ? -kHardY : destinationY > kHardY ? kHardY : destinationY;
  require(x == expectedX, "gaze motion must settle at exact bounded X destination");
  require(y == expectedY, "gaze motion must settle at exact bounded Y destination");
  require(!motion.active(), "gaze motion must return to IDLE after settling");
}

void testGazeEnvelopeAndCompletion() {
  const int16_t xs[] = {-20, -12, 0, 12, 20};
  const int16_t ys[] = {-12, -7, 0, 7, 12};
  const int16_t destinationsX[] = {-24, -20, -16, 0, 16, 20, 24};
  const int16_t destinationsY[] = {-15, -12, -8, 0, 8, 12, 15};

  for (int16_t startX : xs) {
    for (int16_t startY : ys) {
      for (int16_t destinationX : destinationsX) {
        for (int16_t destinationY : destinationsY) {
          runGazeCase(startX, startY, destinationX, destinationY, true);
          runGazeCase(startX, startY, destinationX, destinationY, false);
        }
      }
    }
  }
}
}  // namespace

int main() {
  testPoseTransitionDoesNotRestart();
  testDirectionAwareClosureHandoff();
  testGazeEnvelopeAndCompletion();
  std::cout << "display-animation-host-test: PASS\n";
  return 0;
}
