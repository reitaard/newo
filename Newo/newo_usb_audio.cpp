#include "newo_usb_audio.h"
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>

NewoUsbAudio newoUsbAudio;
namespace {
constexpr uint32_t kRingBytes = 8192;
void error(const char* action, esp_err_t e) {
  Serial.printf("[usb-uac] %s reason=%s\n", action, esp_err_to_name(e));
}
void stringDescriptor(const char* name, const usb_str_desc_t* str) {
  char text[128] = {};
  if (str && str->bLength >= 2) {
    unsigned count = (str->bLength - 2) / 2;
    if (count > sizeof(text) - 1) count = sizeof(text) - 1;
    for (unsigned i = 0; i < count; ++i) {
      const uint16_t c = str->wData[i];
      text[i] = c >= 32 && c < 127 ? char(c) : '?';
    }
  }
  Serial.printf("[usb-uac] %s=%s\n", name, str ? text : "<unavailable>");
}
}
bool NewoUsbAudio::begin(usb_host_client_handle_t client) {
  client_ = client;
  uac_host_driver_config_t config = {};
  config.create_background_task = true;
  config.task_priority = 2;
  config.stack_size = 4096;
  config.core_id = tskNO_AFFINITY;
  // Discovery/diagnostics use the existing monitor client, not a second daemon.
  config.callback = [](uint8_t, uint8_t, uac_host_driver_event_t, void*) {};
  const esp_err_t e = uac_host_install(&config);
  enabled_ = e == ESP_OK;
  if (!enabled_) error("driver unavailable; diagnostics remain available", e);
  Serial.printf("[usb-uac] UAC1 experimental driver=%s; no automatic audio. Commands: uac mic | uac tone | uac duplex | uac stop | uac status\n", enabled_ ? "ready" : "unavailable");
  return enabled_;
}
bool NewoUsbAudio::connected(usb_device_handle_t device, uint8_t address) {
  const usb_config_desc_t* cfg = nullptr;
  esp_err_t e = usb_host_get_active_config_descriptor(device, &cfg);
  if (e != ESP_OK || !cfg) { error("descriptor read failed", e); return false; }
  const auto set = NewoUac::parse(reinterpret_cast<const uint8_t*>(cfg), cfg->wTotalLength);
  Serial.printf("[usb] CLASSIFY address=%u audio=%u msc=%u descriptors=%s\n", address, set.audio, set.msc, set.valid ? "valid" : "malformed");
  if (!set.audio) return false;
  Serial.printf("[usb-uac] connected address=%u\n", address);
  const usb_device_desc_t* desc = nullptr;
  if (usb_host_get_device_descriptor(device, &desc) == ESP_OK)
    Serial.printf("[usb-uac] vid=%04x pid=%04x\n", desc->idVendor, desc->idProduct);
  usb_device_info_t info = {};
  if (usb_host_device_info(device, &info) == ESP_OK) {
    stringDescriptor("manufacturer", info.str_desc_manufacturer);
    stringDescriptor("product", info.str_desc_product);
    Serial.printf("[usb-uac] speed=%s\n", info.speed == USB_SPEED_FULL ? "full-speed (12Mbps)" : "unsupported");
  }
  Serial.printf("[usb-uac] UAC version=0x%04x valid=%u truncated=%u\n", set.version, set.valid, set.overflow);
  // Include control interfaces and every endpoint, including feedback endpoints
  // that the conservative streaming policy intentionally refuses to open.
  const auto* raw = reinterpret_cast<const uint8_t*>(cfg);
  bool audioInterface = false;
  uint8_t iface = 0, alternate = 0;
  for (size_t pos = 0; pos + 2 <= cfg->wTotalLength;) {
    const uint8_t* d = raw + pos;
    if (d[0] < 2 || d[0] > cfg->wTotalLength - pos) break;
    if (d[1] == 4 && d[0] >= 9) {
      audioInterface = d[5] == 1; iface = d[2]; alternate = d[3];
      if (audioInterface) Serial.printf("[usb-uac] interface=%u alt=%u subclass=%u protocol=0x%02x\n", iface, alternate, d[6], d[7]);
    } else if (audioInterface && d[1] == 5 && d[0] >= 7) {
      const char* types[] = {"control", "isochronous", "bulk", "interrupt"};
      Serial.printf("[usb-uac] interface=%u alt=%u endpoint=0x%02x direction=%s transfer=%s MPS=%u usage=%u sync=%u\n", iface, alternate, d[2], (d[2] & 0x80) ? "IN" : "OUT", types[d[3] & 3], d[4] | (unsigned(d[5]) << 8), (d[3] >> 4) & 3, (d[3] >> 2) & 3);
    }
    pos += d[0];
  }
  for (unsigned i = 0; i < set.count; ++i) {
    const auto& a = set.alts[i];
    const char* direction = !a.endpoint ? "AS" : (a.endpoint & 0x80) ? "MIC" : "SPK";
    Serial.printf("[usb-uac] %s interface=%u alt=%u protocol=0x%02x ep=0x%02x transfer=%u attr=0x%02x MPS=%u interval=%u endpoints=%u\n", direction, a.interfaceNumber, a.alternate, a.protocol, a.endpoint, a.attributes & 3, a.attributes, a.mps, a.interval, a.endpoints);
    Serial.printf("[usb-uac] format=%u channels=%u bits=%u subframe=%u rates=%s\n", a.format, a.channels, a.bits, a.subframe, a.protocol ? "clock/control query required; UAC2/3 not decoded as UAC1" : a.continuous ? "continuous range" : "discrete Hz");
    for (unsigned r = 0; r < a.rateCount; ++r) Serial.printf("[usb-uac] rate[%u]=%lu\n", r, static_cast<unsigned long>(a.rates[r]));
    if (const char* reason = a.rejection()) Serial.printf("[usb-uac] interface=%u alt=%u test-unavailable=%s\n", a.interfaceNumber, a.alternate, reason);
    if (a.mps > 128 && (a.endpoint & 0x80)) Serial.printf("[usb-uac] MPS=%u exceeds old default IN=128; configured IN=%u. Descriptor unchanged.\n", a.mps, NewoUac::kInMps);
  }
  if (device_) { Serial.println("[usb-uac] diagnostics only: one physical audio test device at a time"); return false; }
  device_ = device; address_ = address; descriptors_ = set; removed_ = false;
  mic_.passed = spk_.passed = false;
  // Retain monitor reference until unplug so DEV_GONE is always delivered.
  return true;
}
void NewoUsbAudio::disconnected(usb_device_handle_t device) {
  if (device == device_) {
    removed_ = true;
    Serial.printf("[usb-uac] disconnected address=%u; cleanup pending\n", address_);
  }
}
void NewoUsbAudio::deviceEvent(uac_host_device_handle_t, uac_host_device_event_t event, void* arg) {
  auto& s = *static_cast<Stream*>(arg);
  if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) s.gone.store(true);
  if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) s.errors.fetch_add(1);
}
bool NewoUsbAudio::start(Stream& s, bool mic) {
  if (!enabled_ || !device_ || removed_ || s.handle) return false;
  bool found = false;
  for (unsigned i = 0; i < descriptors_.count && !found; ++i)
    found = NewoUac::select(descriptors_, descriptors_.alts[i].interfaceNumber, mic, s.alt, s.rate);
  if (!found) { Serial.printf("[usb-uac] %s no supported test format; inspect alternate diagnostics\n", mic ? "MIC" : "SPK"); return false; }
  s.gone.store(false); s.errors.store(0); s.passed = false;
  uac_host_device_config_t config = {};
  config.addr = address_; config.iface_num = s.alt.interfaceNumber;
  config.buffer_size = kRingBytes; config.buffer_threshold = 2048;
  config.callback = deviceEvent; config.callback_arg = &s;
  esp_err_t e = uac_host_device_open(&config, &s.handle);
  if (e != ESP_OK) { error("open failed", e); return false; }
  uac_host_stream_config_t format = {};
  format.channels = s.alt.channels; format.bit_resolution = 16; format.sample_freq = s.rate;
  e = uac_host_device_start(s.handle, &format);
  if (e != ESP_OK) { error("start failed (claim/control/allocation)", e); stop(s); return false; }
  s.active = true; s.started = s.reported = millis();
  s.bytes = s.lastBytes = s.samples = s.peak = s.toneFrames = s.writeFailures = s.activeUnderruns = 0; s.squares = 0;
  Serial.printf("[usb-uac] %s start interface=%u alt=%u ep=0x%02x %luHz PCM16 channels=%u MPS=%u ring=%lu DMA-payload=%u\n", mic ? "MIC" : "SPK", s.alt.interfaceNumber, s.alt.alternate, s.alt.endpoint, static_cast<unsigned long>(s.rate), s.alt.channels, s.alt.mps, static_cast<unsigned long>(kRingBytes), 9 * s.alt.mps);
  return true;
}
bool NewoUsbAudio::stop(Stream& s) {
  if (!s.handle) return true;
  s.closing = true;
  const esp_err_t e = uac_host_device_close(s.handle);
  if (e != ESP_OK) { error("close pending; retaining handle for retry", e); return false; }
  s.handle = nullptr; s.active = false; s.closing = false; s.gone.store(false);
  return true;
}
void NewoUsbAudio::report(Stream& s, bool mic) {
  if (!s.handle) return;
  newo_uac_stats_t stats = {};
  newo_uac_get_stats(s.handle, &stats);
  const uint32_t now = millis(), elapsed = now - s.reported;
  const uint32_t bps = elapsed ? uint64_t(s.bytes - s.lastBytes) * 1000 / elapsed : 0;
  const double rms = s.samples ? sqrt(double(s.squares) / s.samples) / 32768.0 : 0;
  Serial.printf("[usb-uac] %s bytes/s=%lu effective-Hz=%lu packets=%lu packet-errors/late=%lu dropped=%lu underruns=%lu submit-errors=%lu transfer-errors=%lu write-retries=%lu buffer-HWM=%lu/%lu rms=%.4f peak=%.4f heap=%u internal=%u PSRAM=%u stack=%u\n", mic ? "MIC" : "SPK", static_cast<unsigned long>(bps), static_cast<unsigned long>(bps / (s.alt.channels * 2)), static_cast<unsigned long>(stats.packets), static_cast<unsigned long>(stats.packet_errors), static_cast<unsigned long>(stats.dropped_packets), static_cast<unsigned long>(stats.underruns), static_cast<unsigned long>(stats.submit_errors), static_cast<unsigned long>(s.errors.load()), static_cast<unsigned long>(s.writeFailures), static_cast<unsigned long>(stats.buffer_high_water), static_cast<unsigned long>(kRingBytes), rms, s.peak / 32768.0, ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), ESP.getFreePsram(), uxTaskGetStackHighWaterMark(nullptr));
  s.reported = now; s.lastBytes = s.bytes; s.samples = s.peak = 0; s.squares = 0;
}
void NewoUsbAudio::pumpMic() {
  // Bound work per service pass even if capture outruns this worker.
  for (unsigned n = 0; n < 4; ++n) {
    uint32_t bytes = 0;
    uac_host_device_read(mic_.handle, reinterpret_cast<uint8_t*>(pcm_), sizeof(pcm_), &bytes, 0);
    if (!bytes) break;
    mic_.bytes += bytes;
    for (unsigned i = 0; i < bytes / 2; ++i) {
      const int32_t sample = pcm_[i];
      mic_.squares += int64_t(sample) * sample; mic_.samples++;
      const unsigned peak = sample < 0 ? -sample : sample;
      if (peak > mic_.peak) mic_.peak = peak;
    }
  }
}
void NewoUsbAudio::pumpTone() {
  // Finite one-second 1kHz tone at -30dBFS, then 50ms silence for drain.
  const uint32_t total = spk_.rate + spk_.rate / 20;
  if (spk_.toneFrames >= total) return;
  newo_uac_stats_t stats = {};
  newo_uac_get_stats(spk_.handle, &stats);
  spk_.activeUnderruns = stats.underruns;
  unsigned frames = spk_.rate / 100;
  if (frames > total - spk_.toneFrames) frames = total - spk_.toneFrames;
  for (unsigned i = 0; i < frames; ++i) {
    const uint32_t frame = spk_.toneFrames + i;
    const float ramp = fminf(1.0f, fminf(frame / (spk_.rate * .01f), (spk_.rate - fminf(frame, spk_.rate)) / (spk_.rate * .01f)));
    const int16_t sample = frame < spk_.rate ? int16_t(1036 * ramp * sinf(6.28318530718f * 1000 * frame / spk_.rate)) : 0;
    for (unsigned ch = 0; ch < spk_.alt.channels; ++ch) pcm_[i * spk_.alt.channels + ch] = sample;
  }
  const uint32_t bytes = frames * spk_.alt.channels * 2;
  if (uac_host_device_write(spk_.handle, reinterpret_cast<uint8_t*>(pcm_), bytes, 0) == ESP_OK) {
    spk_.toneFrames += frames; spk_.bytes += bytes;
  } else spk_.writeFailures++;
}
void NewoUsbAudio::command(const char* cmd) {
  if (!strcmp(cmd, "uac status")) {
    Serial.printf("[usb-uac] device=%u MIC=%u SPK=%u individual-MIC-pass=%u individual-SPK-pass=%u\n", address_, mic_.active, spk_.active, mic_.passed, spk_.passed);
  } else if (!strcmp(cmd, "uac stop")) {
    stop(mic_); stop(spk_); duplex_ = false;
  } else if (!strcmp(cmd, "uac mic") || !strcmp(cmd, "uac tone") || !strcmp(cmd, "uac duplex")) {
    if (mic_.handle || spk_.handle) { Serial.println("[usb-uac] busy; uac stop first"); return; }
    duplex_ = !strcmp(cmd, "uac duplex");
    if (duplex_ && (!mic_.passed || !spk_.passed)) { Serial.println("[usb-uac] duplex requires successful individual mic and tone tests first"); duplex_ = false; return; }
    if (duplex_) {
      if (!start(mic_, true) || !start(spk_, false)) { stop(mic_); stop(spk_); duplex_ = false; }
      else Serial.println("[usb-uac] duplex running: independent tone + meter, no loopback");
    } else start(!strcmp(cmd, "uac mic") ? mic_ : spk_, !strcmp(cmd, "uac mic"));
  } else Serial.println("[usb-uac] commands: uac mic | uac tone | uac duplex | uac stop | uac status");
}
void NewoUsbAudio::service() {
  // On unplug the driver callback only marks gone. This worker owns close;
  // USB completion events continue on the driver task while cleanup waits.
  if (mic_.closing) stop(mic_);
  if (spk_.closing) stop(spk_);
  if (removed_ || mic_.gone.load() || spk_.gone.load()) {
    if (mic_.handle && mic_.gone.load()) stop(mic_);
    if (spk_.handle && spk_.gone.load()) stop(spk_);
    if (!mic_.handle && !spk_.handle && device_) {
      const esp_err_t e = usb_host_device_close(client_, device_);
      if (e == ESP_OK) { device_ = nullptr; address_ = 0; removed_ = false; duplex_ = false; Serial.println("[usb-uac] cleanup complete"); }
      else error("monitor close pending", e);
    }
    return;
  }
  // Serial parsing stays off loop(), is bounded, and never accepts partial overflow.
  for (unsigned i = 0; i < 32 && Serial.available(); ++i) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      command_[commandLength_] = 0;
      if (!commandOverflow_ && commandLength_) command(command_);
      commandLength_ = 0; commandOverflow_ = false;
    } else if (commandLength_ < sizeof(command_) - 1) command_[commandLength_++] = c;
    else commandOverflow_ = true;
  }
  if (mic_.active && !mic_.closing) pumpMic();
  if (spk_.active && !spk_.closing) pumpTone();
  const uint32_t now = millis();
  for (Stream* s : {&mic_, &spk_}) {
    if (!s->active || s->closing) continue;
    const bool mic = s == &mic_;
    if (now - s->reported >= 1000) report(*s, mic);
    const uint32_t duration = mic ? (duplex_ ? 3000 : 5000) : 1500;
    if (s->errors.load() || now - s->started >= duration) {
      report(*s, mic);
      newo_uac_stats_t stats = {};
      newo_uac_get_stats(s->handle, &stats);
      const uint32_t expected = uint64_t(now - s->started) * s->rate * s->alt.channels * 2 / 1000;
      const bool passed = !s->errors.load() && !stats.packet_errors && !stats.submit_errors && !stats.dropped_packets && stats.packets > 0 && (mic ? s->bytes >= expected * 9 / 10 : s->toneFrames >= s->rate && !s->activeUnderruns);
      if (!mic) Serial.printf("[usb-uac] SPK starvation-during-generation=%lu; total underruns include finite waveform tail\n", static_cast<unsigned long>(s->activeUnderruns));
      Serial.printf("[usb-uac] %s test=%s; physical audibility/level and timing require observation\n", mic ? "MIC" : "SPK", passed ? "transport-pass" : "failed");
      if (stop(*s) && !duplex_) s->passed = passed;
    }
  }
  if (duplex_ && !mic_.handle && !spk_.handle) { Serial.println("[usb-uac] duplex ended; inspect both counters (not a stability certification)"); duplex_ = false; }
}
