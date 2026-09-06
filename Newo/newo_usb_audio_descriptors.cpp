#include "newo_usb_audio_descriptors.h"

namespace NewoUac {
static uint16_t le16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
static uint32_t le24(const uint8_t* p) { return le16(p) | (uint32_t(p[2]) << 16); }
bool Alt::supports(uint32_t rate) const {
  if (continuous) return rate >= rates[0] && rate <= rates[1];
  for (unsigned i = 0; i < rateCount; ++i) if (rates[i] == rate) return true;
  return false;
}
const char* Alt::rejection() const {
  if (protocol != 0) return "driver supports UAC1 only";
  if (!alternate) return "zero-bandwidth alternate";
  if (endpoints != 1) return "test requires one data endpoint; explicit feedback not supported";
  if ((attributes & 3) != 1) return "not isochronous";
  if ((attributes & 0x30) != 0) return "feedback/implicit-feedback endpoint not supported";
  if (!(endpoint & 0x80) && (attributes & 0x0c) == 0x04)
    return "asynchronous playback requires feedback support absent in this driver";
  if (interval != 1) return "test requires full-speed bInterval=1; descriptor will not be rewritten";
  if (!mps || mps > ((endpoint & 0x80) ? kInMps : kOutMps)) return "endpoint MPS exceeds configured FIFO limit";
  if (format != 1 || subframe != 2 || bits != 16) return "test supports Type-I signed PCM16 in two-byte subframes";
  if (channels < 1 || channels > 2) return "test supports mono/stereo only";
  if (!continuous && !rateCount) return "no sample rates advertised";
  return nullptr;
}
DescriptorSet parse(const uint8_t* data, size_t length) {
  DescriptorSet out;
  if (!data || length < 9 || data[0] < 9 || data[1] != 2 || le16(data + 2) != length) {
    out.valid = false; return out;
  }
  uint8_t cls = 0, sub = 0, protocol = 0;
  Alt* alt = nullptr;
  for (size_t pos = 0; pos < length;) {
    if (length - pos < 2 || data[pos] < 2 || data[pos] > length - pos) { out.valid = false; break; }
    const uint8_t* d = data + pos;
    const unsigned n = d[0];
    if (d[1] == 4) {
      if (n < 9) { out.valid = false; break; }
      cls = d[5]; sub = d[6]; protocol = d[7]; alt = nullptr;
      out.audio |= cls == 1; out.msc |= cls == 8;
      if (cls == 1 && sub == 2) {
        if (out.count == kMaxAlts) out.overflow = true;
        else {
          alt = &out.alts[out.count++];
          alt->interfaceNumber = d[2]; alt->alternate = d[3];
          alt->endpoints = d[4]; alt->protocol = protocol;
        }
      }
    } else if (cls == 1 && d[1] == 0x24) {
      if (n < 3) { out.valid = false; break; }
      if (sub == 1 && d[2] == 1) {
        if (n < 5) { out.valid = false; break; }
        out.version = le16(d + 3);
      } else if (alt && protocol == 0 && d[2] == 1) {
        if (n < 7) { out.valid = false; break; }
        alt->format = le16(d + 5);
      } else if (alt && protocol == 0 && d[2] == 2) {
        if (n < 4) { out.valid = false; break; }
        if (d[3] == 1) {
          if (n < 8 || n < 8u + (d[7] ? d[7] * 3u : 6u)) { out.valid = false; break; }
          alt->channels = d[4]; alt->subframe = d[5]; alt->bits = d[6];
          alt->continuous = d[7] == 0;
          unsigned count = d[7] ? d[7] : 2;
          if (count > kMaxRates) { out.overflow = true; count = kMaxRates; }
          alt->rateCount = count;
          for (unsigned i = 0; i < count; ++i) alt->rates[i] = le24(d + 8 + i * 3);
        }
      }
    } else if (alt && d[1] == 5) {
      if (n < 7) { out.valid = false; break; }
      // Retain data endpoint; bNumEndpoints still exposes additional feedback endpoints.
      if (!alt->endpoint) {
        alt->endpoint = d[2]; alt->attributes = d[3];
        alt->mps = le16(d + 4); alt->interval = d[6];
      }
    }
    pos += n;
  }
  return out;
}
bool select(const DescriptorSet& set, uint8_t iface, bool mic, Alt& chosen, uint32_t& rate) {
  if (!set.valid || set.overflow || set.version != 0x0100) return false;
  const uint32_t micRates[] = {16000, 48000, 32000, 44100, 24000, 8000};
  const uint32_t spkRates[] = {48000, 44100, 32000, 16000, 24000, 8000};
  for (unsigned r = 0; r < 6; ++r) {
    const uint32_t hz = mic ? micRates[r] : spkRates[r];
    for (uint8_t channels = 1; channels <= 2; ++channels) {
      // Match upstream's last discrete match / first continuous match selection.
      const Alt* match = nullptr;
      for (unsigned i = 0; i < set.count; ++i) {
        const Alt& a = set.alts[i];
        if (a.interfaceNumber != iface || !a.alternate || a.channels != channels || a.bits != 16 || !a.supports(hz)) continue;
        match = &a;
        if (a.continuous) break;
      }
      if (!match || match->rejection() || bool(match->endpoint & 0x80) != mic) continue;
      if (((hz + 999) / 1000) * channels * 2 > match->mps) continue;
      chosen = *match; rate = hz; return true;
    }
  }
  return false;
}
}  // namespace NewoUac
