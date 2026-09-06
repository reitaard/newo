#include "../Newo/newo_arduino_wire.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using NewoArduinoWire::Assembler;

static Assembler::Result feed(Assembler& parser, const std::string& bytes, char* frame) {
  Assembler::Result result = Assembler::Result::NONE;
  for (unsigned char byte : bytes) {
    const auto next = parser.push(byte, frame, NewoArduinoWire::kMaxFrameBytes);
    if (next != Assembler::Result::NONE) result = next;
  }
  return result;
}

int main() {
  Assembler parser;
  char frame[NewoArduinoWire::kMaxFrameBytes] = {};

  assert(feed(parser, "NEOWIRE/1 HEL", frame) == Assembler::Result::NONE);
  assert(parser.partialLength() != 0);
  assert(feed(parser, "LO_ACK id=4 version=1 capabilities=gpio,imu\r\n", frame) == Assembler::Result::FRAME);
  assert(std::strcmp(frame, "NEOWIRE/1 HELLO_ACK id=4 version=1 capabilities=gpio,imu") == 0);

  char value[32] = {};
  assert(NewoArduinoWire::field(frame, "id", value, sizeof(value)) && std::strcmp(value, "4") == 0);
  assert(!NewoArduinoWire::field(frame, "missing", value, sizeof(value)));
  assert(NewoArduinoWire::token("read_sensor-2"));
  assert(!NewoArduinoWire::token("bad command"));

  char encoded[128] = {}, decoded[128] = {};
  assert(NewoArduinoWire::encode("two words/%", encoded, sizeof(encoded)));
  assert(std::strcmp(encoded, "two%20words%2F%25") == 0);
  assert(NewoArduinoWire::decode(encoded, decoded, sizeof(decoded)));
  assert(std::strcmp(decoded, "two words/%") == 0);
  assert(!NewoArduinoWire::decode("bad%2", decoded, sizeof(decoded)));

  std::string oversized(NewoArduinoWire::kMaxFrameBytes + 20, 'x');
  oversized += '\n';
  assert(feed(parser, oversized, frame) == Assembler::Result::MALFORMED);
  assert(feed(parser, "NEOWIRE/1 EVENT name=ok payload=ready\n", frame) == Assembler::Result::FRAME);
  assert(feed(parser, std::string("bad\x01input\n", 10), frame) == Assembler::Result::MALFORMED);

  // Every split point preserves a partial frame and emits exactly at LF.
  const std::string complete = "NEOWIRE/1 ACK id=99 status=ok payload=x\n";
  for (size_t split = 0; split < complete.size(); ++split) {
    parser.reset();
    assert(feed(parser, complete.substr(0, split), frame) == Assembler::Result::NONE);
    assert(feed(parser, complete.substr(split), frame) == Assembler::Result::FRAME);
  }

  std::puts("Arduino wire framing tests passed");
}
