// Proof for raw_op.h: the spec parser and the safety class of the generic raw
// operation (VitoHomeComponent::queue_raw_op()).
//
//   A) the documented specs parse into the fields they name;
//   B) every malformed field is refused, naming it;
//   C) the class table: reads and the raw lane's existing writes pass with
//      raw_access: unrestricted, codes without a name are PROBEs without
//      payload, and everything that may change the controller is UNSAFE;
//   D) exhaustively over layer x code x payload: a payload is never READ or
//      PROBE.
//
// Built by build_and_run.sh.

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "raw_op.h"

using namespace esphome::vitohome;

namespace {
int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

constexpr uint8_t kReadMax = 48;

bool parse(const char *text, RawSpec &spec) {
  const char *error = nullptr;
  const bool ok = parse_raw_spec(text, spec, error, kReadMax);
  return ok && error == nullptr;
}

bool refused(const char *text, const char *field) {
  RawSpec spec;
  const char *error = nullptr;
  const bool ok = parse_raw_spec(text, spec, error, kReadMax);
  return !ok && error != nullptr && std::strstr(error, field) == error;
}
}  // namespace

int main() {
  std::printf("== raw_op.h: spec parser + safety class ==\n");
  RawSpec s{};

  // --- A ------------------------------------------------------------------
  check(parse("p300 r 05 1000 16", s) && s.layer == RawLayer::P300 && s.code == 0x05 && s.address == 0x1000 &&
            s.length == 16 && !s.has_payload,
        "A: p300 r 05 1000 16");
  check(parse("kw r 7B 0800 2", s) && s.layer == RawLayer::KW && s.code == 0x7B && s.address == 0x0800 && s.length == 2,
        "A: kw r 7B 0800 2");
  check(parse("gwg r 31 05 1", s) && s.layer == RawLayer::GWG && s.code == 0x31 && s.address == 0x05 && s.length == 1,
        "A: gwg r 31 05 1");
  check(parse("p300 w 02 2306 25", s) && s.has_payload && s.length == 1 && s.payload[0] == 0x25,
        "A: p300 w 02 2306 25");
  check(parse("  VS1  W  0xF4  0x2306  00 0a  0B0c ", s) && s.layer == RawLayer::KW && s.code == 0xF4 &&
            s.address == 0x2306 && s.length == 4 && s.payload[0] == 0x00 && s.payload[1] == 0x0A &&
            s.payload[2] == 0x0B && s.payload[3] == 0x0C,
        "A: case, 0x prefixes, aliases, byte runs, extra spaces");
  check(parse("p300 r 07 2B00 0", s) && s.code == 0x07 && s.length == 0, "A: a call without parameter asks 0");
  check(parse("p300 w 07 A801 00", s) && s.code == 0x07 && s.has_payload && s.length == 1,
        "A: a call with parameter is w");
  check(parse("p300 r 1F FFFF 48", s) && s.code == 0x1F && s.address == 0xFFFF && s.length == kReadMax,
        "A: the edges: code 1F, address FFFF, length = read cap");
  check(parse("gwg r CB FF 1", s) && s.address == 0xFF, "A: GWG address FF");
  {
    char line[160] = "p300 w 02 0000";
    for (int i = 0; i < 32; ++i)
      std::strcat(line, " AB");
    check(parse(line, s) && s.length == 32, "A: 32 payload bytes");
  }

  // --- B ------------------------------------------------------------------
  check(refused("", "empty"), "B: empty");
  check(refused("   ", "empty"), "B: blanks only");
  check(refused("vs3 r 01 0000 1", "layer"), "B: unknown layer");
  check(refused("p300 x 01 0000 1", "direction"), "B: unknown direction");
  check(refused("p300 r 20 0000 1", "code"), "B: P300 code above 1F");
  check(refused("p300 r 00 0000 1", "code"), "B: code 0");
  check(refused("kw r 100 0000 1", "code"), "B: KW code above FF");
  check(refused("kw r G1 0000 1", "code"), "B: code not hex");
  check(refused("p300 r 01 10000 1", "address"), "B: address above FFFF");
  check(refused("gwg r CB 100 1", "address"), "B: GWG address above FF");
  check(refused("p300 r 01", "address"), "B: address missing");
  check(refused("p300 r 01 0000", "length"), "B: length missing");
  check(refused("p300 r 01 0000 0", "length"), "B: length 0 outside a call");
  check(refused("p300 r 07 0000 2", "length"), "B: a call asks 0 without parameter");
  check(refused("p300 r 01 0000 49", "length"), "B: length above the read cap");
  check(refused("p300 r 01 0000 1x", "length"), "B: length not decimal");
  check(refused("p300 r 01 0000 1 2", "trailing"), "B: a field after the length");
  check(refused("p300 w 02 0000", "bytes"), "B: w without bytes");
  check(refused("p300 w 02 0000 ABC", "bytes"), "B: odd hex digits");
  check(refused("p300 w 02 0000 GG", "bytes"), "B: bytes not hex");
  {
    char line[160] = "p300 w 02 0000";
    for (int i = 0; i < 33; ++i)
      std::strcat(line, " AB");
    check(refused(line, "bytes"), "B: 33 payload bytes");
  }
  {
    const char *error = nullptr;
    check(!parse_raw_spec(nullptr, s, error, kReadMax) && error != nullptr, "B: null text");
  }

  // --- C ------------------------------------------------------------------
  check(raw_op_class(RawLayer::P300, 0x01, false) == RawClass::READ, "C: p300 01 read");
  check(raw_op_class(RawLayer::P300, 0x03, false) == RawClass::READ, "C: p300 03 physical read");
  check(raw_op_class(RawLayer::P300, 0x05, false) == RawClass::READ, "C: p300 05 EEPROM read");
  check(raw_op_class(RawLayer::P300, 0x05, true) == RawClass::UNSAFE, "C: p300 05 with payload unsafe");
  check(raw_op_class(RawLayer::P300, 0x02, true) == RawClass::WRITE, "C: p300 02 Virtual_WRITE");
  check(raw_op_class(RawLayer::P300, 0x04, true) == RawClass::UNSAFE, "C: p300 04 physical write unsafe");
  check(raw_op_class(RawLayer::P300, 0x06, false) == RawClass::UNSAFE, "C: p300 06 EEPROM write unsafe");
  check(raw_op_class(RawLayer::P300, 0x07, false) == RawClass::WRITE, "C: p300 07 call as raw rpc");
  check(raw_op_class(RawLayer::P300, 0x09, false) == RawClass::PROBE, "C: p300 09 probe");
  check(raw_op_class(RawLayer::P300, 0x09, true) == RawClass::UNSAFE, "C: p300 09 with payload unsafe");
  check(raw_op_class(RawLayer::KW, 0xF7, false) == RawClass::READ, "C: kw F7 read");
  check(raw_op_class(RawLayer::KW, 0x6B, false) == RawClass::READ, "C: kw 6B GFA read");
  check(raw_op_class(RawLayer::KW, 0x7B, false) == RawClass::READ, "C: kw 7B PROZESS read");
  check(raw_op_class(RawLayer::KW, 0xF4, true) == RawClass::WRITE, "C: kw F4 write");
  check(raw_op_class(RawLayer::KW, 0x68, true) == RawClass::UNSAFE, "C: kw 68 GFA_WRITE unsafe");
  check(raw_op_class(RawLayer::KW, 0x78, false) == RawClass::UNSAFE, "C: kw 78 PROZESS_WRITE unsafe");
  check(raw_op_class(RawLayer::KW, 0x31, false) == RawClass::PROBE, "C: kw 31 probe");
  check(raw_op_class(RawLayer::GWG, 0xAE, false) == RawClass::READ, "C: gwg AE EEPROM read");
  check(raw_op_class(RawLayer::GWG, 0xAD, true) == RawClass::WRITE, "C: gwg AD EEPROM write");
  check(raw_op_class(RawLayer::GWG, 0xAD, false) == RawClass::UNSAFE, "C: gwg AD without payload unsafe");
  check(raw_op_class(RawLayer::GWG, 0x31, false) == RawClass::PROBE, "C: gwg 31 probe");
  check(raw_op_class(RawLayer::NONE, 0x01, false) == RawClass::UNSAFE, "C: no layer unsafe");

  // --- D ------------------------------------------------------------------
  bool payload_never_read = true;
  for (RawLayer layer : {RawLayer::NONE, RawLayer::P300, RawLayer::KW, RawLayer::GWG}) {
    for (int code = 0; code < 256; ++code) {
      const RawClass c = raw_op_class(layer, static_cast<uint8_t>(code), true);
      if (c == RawClass::READ || c == RawClass::PROBE)
        payload_never_read = false;
    }
  }
  check(payload_never_read, "D: a payload is never READ or PROBE, any layer, any code");

  std::printf("raw op: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
