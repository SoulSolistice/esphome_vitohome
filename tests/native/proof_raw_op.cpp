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
//   E) open frames (`<layer> f`): bytes, `[ ]` with CRC-16/KERMIT, `{ }` with
//      the P300 sum, `/ n`, on every layer.
//   F) timed frames: `~ms` waits, `@now`, `/ p300`, and every misplaced one
//      refused.
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

  // --- E ------------------------------------------------------------------
  {
    // The openv wiki's KM-bus example: the ID read at F8 ends in 31 B4.
    const uint8_t wiki[] = {0x01, 0x00, 0x33, 0x0A, 0x01, 0x01, 0xF8, 0x04};
    check(km_crc16(wiki, sizeof(wiki)) == 0xB431, "E: CRC-16/KERMIT of the wiki example is 31 B4");
    const uint8_t check_string[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(km_crc16(check_string, sizeof(check_string)) == 0x2189, "E: CRC-16/KERMIT check value 2189");
    check(km_crc16(nullptr, 0) == 0, "E: CRC of nothing is 0");
    // The P300 read of 0x00F8, two bytes: 41 05 00 01 00 F8 02, checksum 00.
    const uint8_t p300[] = {0x05, 0x00, 0x01, 0x00, 0xF8, 0x02};
    check(p300_sum8(p300, sizeof(p300)) == 0x00, "E: P300 sum of a read of 00F8 is 00");
    const uint8_t answer[] = {0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB};
    check(p300_sum8(answer, sizeof(answer)) == 0xEE, "E: P300 sum of its answer 20 CB is EE");
  }
  check(parse("gwg f 01 CB 6F 01 04 / 1", s) && s.frame && s.layer == RawLayer::GWG && s.frame_len == 5 &&
            s.length == 1 && s.code == 0 && s.address == 0 && !s.has_payload && s.payload[0] == 0x01 &&
            s.payload[1] == 0xCB && s.payload[4] == 0x04,
        "A: gwg f 01 CB 6F 01 04 / 1");
  check(parse("gwg f 01 43 01 01 04", s) && s.frame && s.frame_len == 5 && s.length == 0,
        "A: no / is collect mode (length 0)");
  check(parse("GWG F 0143 0x01 0104 / 0", s) && s.frame && s.frame_len == 5 && s.length == 0,
        "A: case, runs, 0x, an explicit / 0");
  check(parse("gwg f 01 43 [01 00 33 0A 01 01 F8 04] 04", s) && s.frame_len == 13 && s.payload[10] == 0x31 &&
            s.payload[11] == 0xB4 && s.payload[12] == 0x04,
        "A: [ ] appends the CRC, low byte first");
  check(parse("gwg f 01 43 [ 01 00 33 0A 01 01 F8 04 ] 04", s) && s.frame_len == 13 && s.payload[10] == 0x31,
        "A: [ and ] as tokens of their own");
  check(parse("gwg f [01]", s) && s.frame_len == 3, "A: a CRC over one byte");
  {
    char line[160] = "gwg f";
    for (int i = 0; i < 32; ++i)
      std::strcat(line, " AB");
    check(parse(line, s) && s.frame_len == 32, "A: a 32-byte frame");
    std::strcat(line, " / 48");
    check(parse(line, s) && s.length == kReadMax, "A: answer length = read cap");
  }
  check(parse("kw f 01 F7 00 F8 02 / 2", s) && s.frame && s.layer == RawLayer::KW && s.frame_len == 5 &&
            s.length == 2 && s.payload[0] == 0x01,
        "A: kw f 01 F7 00 F8 02 / 2");
  check(parse("p300 f 41 {05 00 01 00 F8 02}", s) && s.frame && s.layer == RawLayer::P300 && s.frame_len == 8 &&
            s.payload[0] == 0x41 && s.payload[6] == 0x02 && s.payload[7] == 0x00 && s.length == 0,
        "A: { } appends the P300 sum");
  check(parse("vs2 f 41 { 07 01 01 00 F8 02 20 CB } / 4", s) && s.layer == RawLayer::P300 && s.frame_len == 10 &&
            s.payload[9] == 0xEE && s.length == 4,
        "A: { and } as tokens, the vs2 alias");
  check(parse("kw f [01 02] {03 04} 05", s) && s.frame_len == 8 && s.payload[4] == 0x03 && s.payload[6] == 0x07 &&
            s.payload[7] == 0x05,
        "A: a [ ] and a { } in one frame");
  check(refused("gwg f", "bytes"), "B: frame without bytes");
  check(refused("gwg f / 2", "bytes"), "B: only a length");
  check(refused("gwg f 01 4", "bytes"), "B: frame byte with one digit");
  check(refused("gwg f 01 GG", "bytes"), "B: frame byte not hex");
  check(refused("gwg f 01 [02 [03] 04]", "bytes"), "B: nested [");
  check(refused("gwg f 01 02]", "bytes"), "B: ] without [");
  check(refused("gwg f 01 []", "bytes"), "B: empty [ ]");
  check(refused("gwg f 01 [02 03", "bytes"), "B: [ without ]");
  check(refused("gwg f 01 [02 / 1", "bytes"), "B: / inside [ ]");
  check(refused("p300 f 41 {05 {06} 07}", "bytes"), "B: nested {");
  check(refused("p300 f 41 [05 {06}]", "bytes"), "B: { inside [ ]");
  check(refused("p300 f 41 {05 06]", "bytes"), "B: { closed by ]");
  check(refused("p300 f 41 [05 06}", "bytes"), "B: [ closed by }");
  check(refused("p300 f 41 {}", "bytes"), "B: empty { }");
  check(refused("p300 f 41 05}", "bytes"), "B: } without {");
  check(refused("p300 f 41 {05", "bytes"), "B: { without }");
  check(refused("gwg f 01 /", "length"), "B: / without a length");
  check(refused("gwg f 01 / 49", "length"), "B: answer above the read cap");
  check(refused("gwg f 01 / 1x", "length"), "B: answer length not decimal");
  check(refused("gwg f 01 / 1 02", "trailing"), "B: a field after the answer length");
  {
    char line[160] = "gwg f";
    for (int i = 0; i < 33; ++i)
      std::strcat(line, " AB");
    check(refused(line, "bytes"), "B: 33 frame bytes");
    char crc_line[160] = "gwg f [";
    for (int i = 0; i < 31; ++i)
      std::strcat(crc_line, " AB");
    std::strcat(crc_line, " ]");
    check(refused(crc_line, "bytes"), "B: 31 bytes and their CRC exceed 32");
    char sum_line[160] = "p300 f {";
    for (int i = 0; i < 31; ++i)
      std::strcat(sum_line, " AB");
    std::strcat(sum_line, " }");
    check(parse(sum_line, s) && s.frame_len == 32, "A: 31 bytes and their sum are 32");
    char over_line[160] = "p300 f {";
    for (int i = 0; i < 32; ++i)
      std::strcat(over_line, " AB");
    std::strcat(over_line, " }");
    check(refused(over_line, "bytes"), "B: 32 bytes and their sum exceed 32");
  }

  // --- C ------------------------------------------------------------------
  check(raw_op_class(RawLayer::P300, 0x01, 0) == RawClass::READ, "C: p300 01 read");
  check(raw_op_class(RawLayer::P300, 0x03, 0) == RawClass::READ, "C: p300 03 physical read");
  check(raw_op_class(RawLayer::P300, 0x05, 0) == RawClass::READ, "C: p300 05 EEPROM read");
  check(raw_op_class(RawLayer::P300, 0x05, 1) == RawClass::UNSAFE, "C: p300 05 with payload unsafe");
  check(raw_op_class(RawLayer::P300, 0x02, 1) == RawClass::WRITE, "C: p300 02 Virtual_WRITE");
  check(raw_op_class(RawLayer::P300, 0x04, 1) == RawClass::UNSAFE, "C: p300 04 physical write unsafe");
  check(raw_op_class(RawLayer::P300, 0x06, 0) == RawClass::UNSAFE, "C: p300 06 EEPROM write unsafe");
  check(raw_op_class(RawLayer::P300, 0x07, 0) == RawClass::WRITE, "C: p300 07 call as raw rpc");
  check(raw_op_class(RawLayer::P300, 0x09, 0) == RawClass::PROBE, "C: p300 09 probe");
  check(raw_op_class(RawLayer::P300, 0x09, 1) == RawClass::UNSAFE, "C: p300 09 with payload unsafe");
  check(raw_op_class(RawLayer::KW, 0xF7, 0) == RawClass::READ, "C: kw F7 read");
  check(raw_op_class(RawLayer::KW, 0x6B, 0) == RawClass::READ, "C: kw 6B GFA read");
  check(raw_op_class(RawLayer::KW, 0x7B, 0) == RawClass::READ, "C: kw 7B PROZESS read");
  check(raw_op_class(RawLayer::KW, 0xF4, 1) == RawClass::WRITE, "C: kw F4 write");
  check(raw_op_class(RawLayer::KW, 0x68, 1) == RawClass::UNSAFE, "C: kw 68 GFA_WRITE unsafe");
  check(raw_op_class(RawLayer::KW, 0x78, 0) == RawClass::UNSAFE, "C: kw 78 PROZESS_WRITE unsafe");
  check(raw_op_class(RawLayer::KW, 0x31, 0) == RawClass::PROBE, "C: kw 31 probe");
  check(raw_op_class(RawLayer::GWG, 0xAE, 0) == RawClass::READ, "C: gwg AE EEPROM read");
  check(raw_op_class(RawLayer::GWG, 0xAD, 1) == RawClass::WRITE, "C: gwg AD EEPROM write");
  check(raw_op_class(RawLayer::GWG, 0xAD, 0) == RawClass::UNSAFE, "C: gwg AD without payload unsafe");
  check(raw_op_class(RawLayer::GWG, 0xAD, 2) == RawClass::UNSAFE, "C: gwg AD pair as given unsafe");
  check(raw_op_class(RawLayer::GWG, 0xC4, 2) == RawClass::WRITE, "C: gwg C4 two-byte write");
  check(raw_op_class(RawLayer::GWG, 0x31, 0) == RawClass::PROBE, "C: gwg 31 probe");
  check(raw_op_class(RawLayer::NONE, 0x01, 0) == RawClass::UNSAFE, "C: no layer unsafe");

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

  // --- F: timed frames ----------------------------------------------------
  check(parse("gwg f ~30 01 AE 16 01 04 / 2", s) && s.frame && s.frame_len == 5 && s.waits == 1 && s.wait_at[0] == 0 &&
            s.wait_ms[0] == 30 && s.length == 2 && !s.now && !s.answer_p300,
        "F: gwg f ~30 01 AE 16 01 04 / 2, a leading wait");
  check(parse("gwg f 01 ~30 AE 16 01 04 / 2", s) && s.waits == 1 && s.wait_at[0] == 1 && s.wait_ms[0] == 30,
        "F: gwg f 01 ~30 AE ..., a wait after the 01");
  check(parse("kw f @now 01 ~20 F7 00 ~10 F8 02 / 2", s) && s.now && s.waits == 2 && s.wait_at[1] == 3 &&
            s.wait_ms[1] == 10 && s.frame_len == 5,
        "F: kw f @now with two waits");
  check(parse("gwg f @NOW 01 CB 6F 01 04", s) && s.now && s.waits == 0 && s.length == 0, "F: @NOW in capitals");
  check(parse("p300 f ~10 41 {05 00 01 00 F8 02} / p300", s) && s.answer_p300 && s.length == 0 && s.frame_len == 8 &&
            s.wait_at[0] == 0,
        "F: p300 f ~10 41 {...} / p300");
  check(parse("p300 f 41 {05 00 01 00 F8 02} / P300", s) && s.answer_p300, "F: / P300 in capitals");
  check(parse("gwg f 01 ~255 CB 6F 01 04", s) && s.wait_ms[0] == 255, "F: ~255 is the longest wait");
  check(parse("gwg f 01 ~1 CB ~2 6F ~3 01 ~4 04", s) && s.waits == 4, "F: four waits");
  check(raw_frame_timed(1, false, false) && raw_frame_timed(0, true, false) && raw_frame_timed(0, false, true) &&
            !raw_frame_timed(0, false, false),
        "F: a frame is timed by a wait, @now or / p300");
  check(parse("gwg f 01 CB 6F 01 04 / 1", s) && !raw_frame_timed(s.waits, s.now, s.answer_p300),
        "F: a plain frame stays on the engine");
  check(refused("gwg f 01 ~0 CB", "wait:"), "F: ~0 refused");
  check(refused("gwg f 01 ~256 CB", "wait:"), "F: ~256 refused");
  check(refused("gwg f 01 ~ CB", "wait:"), "F: ~ without a number refused");
  check(refused("gwg f 01 ~3x CB", "wait:"), "F: ~3x refused");
  check(refused("gwg f 01 ~10 ~20 CB", "wait:"), "F: two waits in a row refused");
  check(refused("gwg f 01 CB ~10", "wait:"), "F: a wait at the end refused");
  check(refused("gwg f 01 CB ~10 / 1", "wait:"), "F: a wait before the length refused");
  check(refused("gwg f 01 ~1 CB ~2 6F ~3 01 ~4 04 ~5 05", "wait:"), "F: a fifth wait refused");
  check(refused("p300 f 41 {05 ~10 00 01 00 F8 02}", "wait:"), "F: a wait inside { } refused");
  check(refused("gwg f [01 ~10 43] 04", "wait:"), "F: a wait inside [ ] refused");
  check(refused("gwg f 01 @now CB", "@now:"), "F: @now after a byte refused");
  check(refused("gwg f ~10 @now 01", "@now:"), "F: @now after a wait refused");
  check(refused("p300 f @now 41 {05 00 01 00 F8 02}", "@now:"), "F: @now on p300 refused");
  check(refused("gwg f 01 CB 6F 01 04 / p300", "length:"), "F: / p300 on gwg refused");
  check(refused("kw f 01 F7 00 F8 02 / p300", "length:"), "F: / p300 on kw refused");
  check(refused("p300 f 41 {05 00 01 00 F8 02} / p300 2", "trailing"), "F: a field after / p300 refused");
  check(refused("gwg f ~10", "bytes:"), "F: a wait alone has no bytes");

  std::printf("raw op: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
