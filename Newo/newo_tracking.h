#pragma once

#include <Arduino.h>

class NewoTracking {
 public:
  enum class State : uint8_t { OFF, ACTIVE };
  struct Result { bool applied; bool duplicate; const char* error; };
  struct Metrics {
    uint32_t callbacks, accepted, rateGateDrops, ringDrops, transportDrops, sent, ringHighWater;
    uint32_t freeHeap, minFreeHeap, freePsram, taskStackBytes;
    const char* collectorSource;
    const char* espNowState;
  };

  void begin();
  void loop();
  Result apply(const char* action, const char* epoch, uint32_t sequence);
  State state() const { return state_; }
  Metrics metrics() const;

 private:
  bool start();
  bool stop();
  State state_ = State::OFF;
  char commandEpoch_[40] = {};
  uint32_t commandSequence_ = 0;
  bool lastApplied_ = false;
};
