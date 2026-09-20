#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Fixed-size voice PCM frame ring. Thread safety is deliberately owned by the
// caller so the firmware can use one cross-core critical section around each
// push/pop. When full, push() discards the oldest frame to keep latency bounded.
//
// The voice path enables a first-order ~20 Hz DC blocker by default before PCM
// enters the ring. This removes the repeatable INMP441/I2S startup offset without
// adding a startup delay or touching useful speech frequencies. Host tests that
// need exact byte-for-byte ring semantics can disable it in the constructor.
class NewoPcmFrameRing {
 public:
  static constexpr int32_t kDcAlphaQ15 = 32512;  // 0.9921875 => ~19.97 Hz at 16 kHz.

  NewoPcmFrameRing(int16_t* storage, size_t capacityFrames, size_t samplesPerFrame,
                   bool dcBlockerEnabled = true)
      : storage_(storage),
        capacityFrames_(capacityFrames),
        samplesPerFrame_(samplesPerFrame),
        dcBlockerEnabled_(dcBlockerEnabled) {}

  bool valid() const { return storage_ && capacityFrames_ > 0 && samplesPerFrame_ > 0; }
  size_t size() const { return count_; }
  size_t capacity() const { return capacityFrames_; }
  uint32_t overwrittenFrames() const { return overwrittenFrames_; }
  uint32_t pushedFrames() const { return pushedFrames_; }
  uint32_t poppedFrames() const { return poppedFrames_; }
  bool dcBlockerEnabled() const { return dcBlockerEnabled_; }

  void reset() {
    writeFrame_ = 0;
    readFrame_ = 0;
    count_ = 0;
    overwrittenFrames_ = 0;
    pushedFrames_ = 0;
    poppedFrames_ = 0;
    dcInitialized_ = false;
    dcPreviousInput_ = 0;
    dcPreviousOutput_ = 0;
  }

  // Returns true when the oldest frame had to be overwritten.
  bool push(const int16_t* frame) {
    if (!valid() || !frame) return false;
    bool overwritten = false;
    if (count_ == capacityFrames_) {
      readFrame_ = (readFrame_ + 1) % capacityFrames_;
      --count_;
      ++overwrittenFrames_;
      overwritten = true;
    }

    int16_t* destination = storage_ + writeFrame_ * samplesPerFrame_;
    if (dcBlockerEnabled_) {
      for (size_t i = 0; i < samplesPerFrame_; ++i) {
        const int32_t input = frame[i];
        if (!dcInitialized_) {
          // Seed from the first physical sample instead of zero. A large startup
          // DC offset therefore cannot become an artificial impulse in ASR PCM.
          dcPreviousInput_ = input;
          dcPreviousOutput_ = 0;
          dcInitialized_ = true;
          destination[i] = 0;
          continue;
        }

        const int32_t output =
            input - dcPreviousInput_ +
            static_cast<int32_t>((static_cast<int64_t>(kDcAlphaQ15) * dcPreviousOutput_) >> 15);
        dcPreviousInput_ = input;
        dcPreviousOutput_ = output;
        destination[i] = clampPcm16(output);
      }
    } else {
      memcpy(destination, frame, samplesPerFrame_ * sizeof(int16_t));
    }

    writeFrame_ = (writeFrame_ + 1) % capacityFrames_;
    ++count_;
    ++pushedFrames_;
    return overwritten;
  }

  // Pops up to maxFrames in strict oldest-to-newest order.
  size_t pop(int16_t* output, size_t maxFrames) {
    if (!valid() || !output || maxFrames == 0 || count_ == 0) return 0;
    const size_t frames = maxFrames < count_ ? maxFrames : count_;
    for (size_t frame = 0; frame < frames; ++frame) {
      memcpy(output + frame * samplesPerFrame_,
             storage_ + readFrame_ * samplesPerFrame_,
             samplesPerFrame_ * sizeof(int16_t));
      readFrame_ = (readFrame_ + 1) % capacityFrames_;
    }
    count_ -= frames;
    poppedFrames_ += static_cast<uint32_t>(frames);
    return frames;
  }

 private:
  static int16_t clampPcm16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return static_cast<int16_t>(value);
  }

  int16_t* storage_ = nullptr;
  size_t capacityFrames_ = 0;
  size_t samplesPerFrame_ = 0;
  size_t writeFrame_ = 0;
  size_t readFrame_ = 0;
  size_t count_ = 0;
  uint32_t overwrittenFrames_ = 0;
  uint32_t pushedFrames_ = 0;
  uint32_t poppedFrames_ = 0;
  bool dcBlockerEnabled_ = true;
  bool dcInitialized_ = false;
  int32_t dcPreviousInput_ = 0;
  int32_t dcPreviousOutput_ = 0;
};
