#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "../Newo/newo_pcm_ring.h"

int main() {
  {
    int16_t storage[3 * 2] = {};
    // Disable input conditioning here because this block validates only ring
    // ordering/overwrite semantics byte-for-byte.
    NewoPcmFrameRing ring(storage, 3, 2, false);
    assert(ring.valid());
    assert(ring.capacity() == 3);
    assert(!ring.dcBlockerEnabled());

    const int16_t a[2] = {1, 2};
    const int16_t b[2] = {3, 4};
    const int16_t c[2] = {5, 6};
    const int16_t d[2] = {7, 8};
    assert(!ring.push(a));
    assert(!ring.push(b));
    assert(!ring.push(c));
    assert(ring.size() == 3);
    assert(ring.push(d));
    assert(ring.size() == 3);
    assert(ring.overwrittenFrames() == 1);

    int16_t output[3 * 2] = {};
    assert(ring.pop(output, 3) == 3);
    const int16_t expected[6] = {3, 4, 5, 6, 7, 8};
    for (size_t i = 0; i < 6; ++i) assert(output[i] == expected[i]);
    assert(ring.size() == 0);
    assert(ring.pushedFrames() == 4);
    assert(ring.poppedFrames() == 3);

    ring.reset();
    assert(ring.size() == 0);
    assert(ring.overwrittenFrames() == 0);
    assert(!ring.push(a));
    assert(!ring.push(b));
    int16_t batch[4] = {};
    assert(ring.pop(batch, 2) == 2);
    assert(batch[0] == 1 && batch[1] == 2);
    assert(batch[2] == 3 && batch[3] == 4);
  }

  {
    // Reproduce the measured startup shape: a several-thousand-count DC offset
    // decaying over roughly a second, with normal 220 Hz speech energy on top.
    // The default voice ring must remove the offset without erasing speech.
    constexpr size_t kSampleRate = 16000;
    constexpr size_t kSamplesPerFrame = 320;
    constexpr size_t kFrames = 50;  // one second
    constexpr double kPi = 3.14159265358979323846;

    std::vector<int16_t> storage(kFrames * kSamplesPerFrame);
    std::vector<int16_t> output(kFrames * kSamplesPerFrame);
    NewoPcmFrameRing ring(storage.data(), kFrames, kSamplesPerFrame);
    assert(ring.dcBlockerEnabled());

    double inputMeanFirstHalf = 0.0;
    for (size_t frameIndex = 0; frameIndex < kFrames; ++frameIndex) {
      int16_t frame[kSamplesPerFrame];
      for (size_t i = 0; i < kSamplesPerFrame; ++i) {
        const size_t sampleIndex = frameIndex * kSamplesPerFrame + i;
        const double t = static_cast<double>(sampleIndex) / kSampleRate;
        const double startupDc = -5000.0 * std::exp(-t / 0.55);
        const double speech = 1000.0 * std::sin(2.0 * kPi * 220.0 * t);
        frame[i] = static_cast<int16_t>(std::lround(startupDc + speech));
        if (sampleIndex < kSampleRate / 2) inputMeanFirstHalf += frame[i];
      }
      assert(!ring.push(frame));
    }

    assert(ring.pop(output.data(), kFrames) == kFrames);
    inputMeanFirstHalf /= static_cast<double>(kSampleRate / 2);

    double outputMeanFirstHalf = 0.0;
    double outputSquareFirstHalf = 0.0;
    for (size_t i = 0; i < kSampleRate / 2; ++i) {
      outputMeanFirstHalf += output[i];
      outputSquareFirstHalf += static_cast<double>(output[i]) * output[i];
    }
    outputMeanFirstHalf /= static_cast<double>(kSampleRate / 2);
    const double outputRmsFirstHalf =
        std::sqrt(outputSquareFirstHalf / static_cast<double>(kSampleRate / 2));

    assert(std::abs(inputMeanFirstHalf) > 3000.0);
    assert(std::abs(outputMeanFirstHalf) < 100.0);
    assert(outputRmsFirstHalf > 600.0);  // 220 Hz speech component remains.
    assert(outputRmsFirstHalf < 850.0);
  }

  std::cout << "voice pcm ring + dc blocker: ok\n";
  return 0;
}
