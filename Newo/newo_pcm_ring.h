#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Fixed-size PCM frame ring. Thread safety is deliberately owned by the caller
// so the firmware can use one cross-core critical section around each push/pop.
// When full, push() discards the oldest frame to keep latency bounded.
class NewoPcmFrameRing {
 public:
  NewoPcmFrameRing(int16_t* storage, size_t capacityFrames, size_t samplesPerFrame)
      : storage_(storage), capacityFrames_(capacityFrames), samplesPerFrame_(samplesPerFrame) {}

  bool valid() const { return storage_ && capacityFrames_ > 0 && samplesPerFrame_ > 0; }
  size_t size() const { return count_; }
  size_t capacity() const { return capacityFrames_; }
  uint32_t overwrittenFrames() const { return overwrittenFrames_; }
  uint32_t pushedFrames() const { return pushedFrames_; }
  uint32_t poppedFrames() const { return poppedFrames_; }

  void reset() {
    writeFrame_ = 0;
    readFrame_ = 0;
    count_ = 0;
    overwrittenFrames_ = 0;
    pushedFrames_ = 0;
    poppedFrames_ = 0;
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
    memcpy(storage_ + writeFrame_ * samplesPerFrame_, frame,
           samplesPerFrame_ * sizeof(int16_t));
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
  int16_t* storage_ = nullptr;
  size_t capacityFrames_ = 0;
  size_t samplesPerFrame_ = 0;
  size_t writeFrame_ = 0;
  size_t readFrame_ = 0;
  size_t count_ = 0;
  uint32_t overwrittenFrames_ = 0;
  uint32_t pushedFrames_ = 0;
  uint32_t poppedFrames_ = 0;
};
