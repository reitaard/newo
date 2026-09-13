#include <cassert>
#include <cstdint>
#include <iostream>

#include "../Newo/newo_pcm_ring.h"

int main() {
  int16_t storage[3 * 2] = {};
  NewoPcmFrameRing ring(storage, 3, 2);
  assert(ring.valid());
  assert(ring.capacity() == 3);

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

  std::cout << "voice pcm ring: ok\n";
  return 0;
}
