#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace NewoArduinoWire {

constexpr size_t kMaxFrameBytes = 256;

class Assembler {
 public:
  enum class Result : uint8_t { NONE, FRAME, MALFORMED };

  Result push(uint8_t byte, char* output, size_t capacity) {
    if (byte == '\n') {
      if (discard_) { reset(); return Result::MALFORMED; }
      if (length_ == 0) return Result::NONE;
      if (buffer_[length_ - 1] == '\r') --length_;
      if (length_ == 0 || output == nullptr || capacity <= length_) { reset(); return Result::MALFORMED; }
      memcpy(output, buffer_, length_);
      output[length_] = '\0';
      reset();
      return Result::FRAME;
    }
    if (discard_) return Result::NONE;
    if ((byte < 0x20 && byte != '\r') || byte > 0x7e || length_ + 1 >= sizeof(buffer_)) {
      discard_ = true;
      return Result::NONE;
    }
    buffer_[length_++] = static_cast<char>(byte);
    return Result::NONE;
  }

  void reset() { length_ = 0; discard_ = false; }
  size_t partialLength() const { return length_; }

 private:
  char buffer_[kMaxFrameBytes] = {};
  size_t length_ = 0;
  bool discard_ = false;
};

inline bool token(const char* value) {
  if (value == nullptr || *value == '\0') return false;
  for (size_t i = 0; value[i] != '\0'; ++i)
    if (!(isalnum(static_cast<unsigned char>(value[i])) || value[i] == '_' || value[i] == '-')) return false;
  return true;
}

inline const char* field(const char* frame, const char* key, char* out, size_t capacity) {
  if (frame == nullptr || key == nullptr || out == nullptr || capacity == 0) return nullptr;
  char needle[40];
  const int count = snprintf(needle, sizeof(needle), "%s=", key);
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(needle)) return nullptr;
  const char* start = strstr(frame, needle);
  if (start == nullptr || (start != frame && start[-1] != ' ')) return nullptr;
  start += strlen(needle);
  const char* end = strchr(start, ' ');
  const size_t length = end == nullptr ? strlen(start) : static_cast<size_t>(end - start);
  if (length >= capacity) return nullptr;
  memcpy(out, start, length);
  out[length] = '\0';
  return out;
}

inline bool encode(const char* input, char* output, size_t capacity) {
  static const char hex[] = "0123456789ABCDEF";
  if (output == nullptr || capacity == 0) return false;
  size_t used = 0;
  for (size_t i = 0; input != nullptr && input[i] != '\0'; ++i) {
    const uint8_t ch = static_cast<uint8_t>(input[i]);
    const bool plain = isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '~';
    const size_t needed = plain ? 1 : 3;
    if (used + needed >= capacity) return false;
    if (plain) output[used++] = static_cast<char>(ch);
    else { output[used++] = '%'; output[used++] = hex[ch >> 4]; output[used++] = hex[ch & 15]; }
  }
  output[used] = '\0';
  return true;
}

inline int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

inline bool decode(const char* input, char* output, size_t capacity) {
  if (output == nullptr || capacity == 0) return false;
  size_t used = 0;
  for (size_t i = 0; input != nullptr && input[i] != '\0'; ++i) {
    int value = static_cast<uint8_t>(input[i]);
    if (input[i] == '%') {
      const int high = hexValue(input[i + 1]);
      const int low = input[i + 1] == '\0' ? -1 : hexValue(input[i + 2]);
      if (high < 0 || low < 0) return false;
      value = (high << 4) | low;
      i += 2;
    }
    if (value == 0 || used + 1 >= capacity) return false;
    output[used++] = static_cast<char>(value);
  }
  output[used] = '\0';
  return true;
}

}  // namespace NewoArduinoWire
