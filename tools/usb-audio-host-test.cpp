#include "../Newo/newo_usb_audio_descriptors.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace NewoUac;
static void add(std::vector<uint8_t>& v, std::initializer_list<uint8_t> d) { v.insert(v.end(), d); }
static std::vector<uint8_t> fixture(uint8_t ep = 0x81, uint8_t attr = 5) {
  std::vector<uint8_t> v = {9,2,0,0,3,1,0,0x80,50};
  add(v, {9,4,0,0,0,1,1,0,0});
  add(v, {9,0x24,1,0,1,9,0,1,2});
  add(v, {9,4,2,0,0,1,2,0,0});
  // Non-contiguous alternate number, representative 208-byte endpoint.
  add(v, {9,4,2,3,1,1,2,0,0});
  add(v, {7,0x24,1,1,1,1,0});
  add(v, {14,0x24,2,1,1,2,16,2,0x80,0x3e,0,0x80,0xbb,0});
  add(v, {9,5,ep,attr,208,0,1,0,0});
  add(v, {7,0x25,1,1,0,0,0});
  add(v, {9,4,4,0,2,8,6,0x50,0});
  v[2] = v.size() & 255; v[3] = v.size() >> 8;
  return v;
}
int main() {
  auto bytes = fixture();
  auto d = parse(bytes.data(), bytes.size());
  assert(d.valid && d.audio && d.msc && d.version == 0x100 && d.count == 2);
  Alt a; uint32_t hz = 0;
  assert(select(d,2,true,a,hz) && hz == 16000 && a.mps == 208 && a.alternate == 3);
  assert(!select(d,2,false,a,hz));
  assert(kInMps == 504 && kOutMps == 384 && kNptxLines * 4 >= 64);
  d.alts[1].mps = 505; assert(!select(d,2,true,a,hz));
  d.alts[1].mps = 208; d.alts[1].interval = 2; assert(!select(d,2,true,a,hz));
  d.alts[1].interval = 1; d.alts[1].endpoints = 2; assert(!select(d,2,true,a,hz));
  d.alts[1].endpoints = 1; d.alts[1].protocol = 0x20; assert(!select(d,2,true,a,hz));
  bytes = fixture(1,9); d = parse(bytes.data(), bytes.size());
  assert(select(d,2,false,a,hz) && hz == 48000);
  d.alts[1].attributes = 5; assert(!select(d,2,false,a,hz)); // async OUT needs feedback
  d.alts[1].attributes = 9; d.alts[1].subframe = 3; assert(!select(d,2,false,a,hz));
  d = parse(bytes.data(),bytes.size());
  d.alts[1].continuous = true; d.alts[1].rates[0] = 8000; d.alts[1].rates[1] = 48000;
  assert(select(d,2,false,a,hz) && hz == 48000);
  // Reject an oversized *selected* later alternate rather than silently
  // assuming that the driver's same-format choice is the earlier safe one.
  d.alts[1].continuous = false; d.alts[2] = d.alts[1]; d.alts[2].mps = 900; d.count = 3;
  assert(!select(d,2,false,a,hz));
  for (size_t i = 0; i < bytes.size(); ++i) assert(!parse(bytes.data(),i).valid);
  for (size_t i = 9; i < bytes.size();) {
    auto bad = bytes; bad[i] = 0; assert(!parse(bad.data(), bad.size()).valid);
    bad[i] = 255; assert(!parse(bad.data(), bad.size()).valid);
    i += bytes[i];
  }
  // Every byte value at every position must remain bounded/terminate.
  for (size_t i = 0; i < bytes.size(); ++i) for (unsigned b = 0; b < 256; ++b) {
    auto changed = bytes; changed[i] = b; (void)parse(changed.data(), changed.size());
  }
  puts("USB audio descriptor/policy tests passed");
}
