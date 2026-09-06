#pragma once
#include <stddef.h>
#include <stdint.h>

namespace NewoUac {
constexpr unsigned kMaxAlts = 16;
constexpr unsigned kMaxRates = 16;
// ESP32-S3 DWC host FIFO budget is 200 lines, four bytes each. Keep enough
// RX capacity for the observed 208-byte UAC IN endpoint while preserving
// non-periodic OUT for MSC/control and periodic OUT for USB speaker tests.
constexpr unsigned kRxLines = 72;
constexpr unsigned kNptxLines = 32;
constexpr unsigned kPtxLines = 96;
constexpr unsigned kFifoLinesTotal = kRxLines + kNptxLines + kPtxLines;
constexpr unsigned kInMps = (kRxLines - 2) * 4;
constexpr unsigned kOutMps = kPtxLines * 4;
static_assert(kFifoLinesTotal == 200, "ESP32-S3 USB host FIFO budget");
struct Alt {
  uint8_t interfaceNumber = 0, alternate = 0, protocol = 0;
  uint8_t endpoint = 0, attributes = 0, interval = 0, endpoints = 0;
  uint16_t mps = 0, format = 0;
  uint8_t channels = 0, subframe = 0, bits = 0, rateCount = 0;
  bool continuous = false;
  uint32_t rates[kMaxRates] = {};
  bool supports(uint32_t rate) const;
  const char* rejection() const;
};
struct DescriptorSet {
  bool audio = false, msc = false, valid = true, overflow = false;
  uint16_t version = 0;
  unsigned count = 0;
  Alt alts[kMaxAlts];
};
// Diagnostics and conservative test-format policy only; transfers use Espressif UAC.
DescriptorSet parse(const uint8_t* data, size_t length);
bool select(const DescriptorSet& set, uint8_t iface, bool mic, Alt& alt, uint32_t& rate);
}  // namespace NewoUac
