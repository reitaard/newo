#pragma once

#include <Arduino.h>

class NewoTracking {
 public:
  enum class State : uint8_t { OFF, ACTIVE };
  struct Result { bool applied; bool duplicate; const char* error; const char* peerStatus; };
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
  bool stop(bool coordinatePeer = true);
  void pollLocalControl();
  void expireLocalControl();
  Result applyLocal(const char* state, const char* session, uint32_t sequence, uint32_t leaseMs);
  static void peerMonitorEntry(void* context);
  void peerMonitorLoop();
  State state_ = State::OFF;
  char commandEpoch_[40] = {};
  uint32_t commandSequence_ = 0;
  bool lastApplied_ = false;
  const char* lastError_ = nullptr;
  const char* volatile peerStatus_ = "stopped";
  char localSession_[65] = {};
  char releasedLocalSession_[65] = {};
  uint32_t localCommandSequence_ = 0;
  uint32_t releasedLocalSequence_ = 0;
  uint32_t localLeaseUntilMs_ = 0;
  bool localLastApplied_ = false;
  const char* localLastError_ = nullptr;
};
