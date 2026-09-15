#pragma once
#include "newo2_event.h"
namespace MotionSnapshotSkill {
bool begin();
bool enqueue(const Newo2Events::Event &event);
}  // namespace MotionSnapshotSkill
