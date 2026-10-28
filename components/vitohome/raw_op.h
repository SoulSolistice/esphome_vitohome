#pragma once

// The generic raw operation (VitoHomeComponent::queue_raw_op()): one telegram
// of any protocol layer with any function code / type byte, for reverse
// engineering. This header holds what does not need the hub -- the spec
// parser and the safety class of an operation -- so the native suite can pin
// both (tests/native/proof_raw_op.cpp). Header-only, like decode.h.
//
// A spec is one line of whitespace-separated fields:
//
//   <layer> r <code> <address> <length>      a telegram without payload
//   <layer> w <code> <address> <bytes...>    a telegram carrying payload
//
//   layer    p300 (vs2), kw (vs1), gwg
//   code     hex: the P300 function code (1..1F, the low five bits of the
//            flags byte), the KW type byte, the GWG TYPE byte
//   address  hex, 0..FFFF (GWG: 0..FF)
//   length   decimal, the bytes asked for (P300 code 07 may ask for 0)
//   bytes    hex, as separate bytes ("00 0A") or runs ("000A")
//
//   p300 r 05 1000 16      EEPROM read, 16 bytes at 0x1000
//   kw r 7B 0800 2         PROZESS_READ (over P300: the KW detour)
//   gwg r 31 05 1          an unnamed GWG TYPE byte
//   p300 w 02 2306 25      Virtual_WRITE of one byte

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "optolink/constants.h"

namespace esphome::vitohome {

// Which protocol's telegram a raw operation is. NONE marks every raw-lane
// operation that is not one (queue_raw_read() & co.); it is 0 so a
// value-initialized RawOp carries it.
enum class RawLayer : uint8_t { NONE = 0, P300, KW, GWG };

// How an operation is gated. READ and WRITE are the telegrams this project
// already sends (a read in a known space, Virtual_WRITE, a known GWG write
// mode, a remote procedure call); PROBE a code with no name here, sent
// without payload -- the controller refuses what it lacks (P300 error 05);
// UNSAFE everything that may change the controller: a payload with an unknown
// code, a known write telegram outside the ones above (P300 physical / EEPROM
// write, KW GFA_WRITE / PROZESS_WRITE), a payload on a read code.
enum class RawClass : uint8_t { READ, PROBE, WRITE, UNSAFE };

inline constexpr std::size_t RAW_OP_PAYLOAD_MAX = 32;

struct RawSpec {
  RawLayer layer;
  uint8_t code;
  uint16_t address;
  // The bytes asked for (r), or the payload's length (w).
  uint8_t length;
  bool has_payload;
  uint8_t payload[RAW_OP_PAYLOAD_MAX];
};

inline const char *raw_layer_name(RawLayer layer) {
  switch (layer) {
    case RawLayer::P300:
      return "p300";
    case RawLayer::KW:
      return "kw";
    case RawLayer::GWG:
      return "gwg";
    default:
      return "none";
  }
}

inline const char *raw_class_name(RawClass cls) {
  switch (cls) {
    case RawClass::READ:
      return "read";
    case RawClass::PROBE:
      return "probe";
    case RawClass::WRITE:
      return "write";
    default:
      return "unsafe";
  }
}

inline RawClass raw_op_class(RawLayer layer, uint8_t code, bool has_payload) {
  switch (layer) {
    case RawLayer::P300:
      // 01 Virtual_READ, 03 Physical_READ, 05 EEPROM_READ; 02 Virtual_WRITE;
      // 04 / 06 their physical / EEPROM writes; 07 the remote procedure call,
      // which queue_raw_rpc() sends ungated.
      if (code == 0x07)
        return RawClass::WRITE;
      if (code == 0x01 || code == 0x03 || code == 0x05)
        return has_payload ? RawClass::UNSAFE : RawClass::READ;
      if (code == 0x02)
        return has_payload ? RawClass::WRITE : RawClass::UNSAFE;
      if (code == 0x04 || code == 0x06)
        return RawClass::UNSAFE;
      break;
    case RawLayer::KW:
      // F7 Virtual_READ, 6B GFA_READ, 7B PROZESS_READ; F4 Virtual_WRITE;
      // 68 GFA_WRITE, 78 PROZESS_WRITE.
      if (code == 0xF7 || code == 0x6B || code == 0x7B)
        return has_payload ? RawClass::UNSAFE : RawClass::READ;
      if (code == 0xF4)
        return has_payload ? RawClass::WRITE : RawClass::UNSAFE;
      if (code == 0x68 || code == 0x78)
        return RawClass::UNSAFE;
      break;
    case RawLayer::GWG:
      // The access-mode table (constants.h): what queue_raw_read() /
      // queue_raw_write() send in some mode.
      if (optolink::isKnownGwgReadType(code))
        return has_payload ? RawClass::UNSAFE : RawClass::READ;
      if (optolink::isKnownGwgWriteType(code))
        return has_payload ? RawClass::WRITE : RawClass::UNSAFE;
      break;
    default:
      return RawClass::UNSAFE;
  }
  return has_payload ? RawClass::UNSAFE : RawClass::PROBE;
}

namespace raw_op_detail {

inline bool next_token(const char *&p, const char *&begin, std::size_t &len) {
  while (*p != '\0' && std::isspace(static_cast<unsigned char>(*p)))
    ++p;
  if (*p == '\0')
    return false;
  begin = p;
  while (*p != '\0' && !std::isspace(static_cast<unsigned char>(*p)))
    ++p;
  len = static_cast<std::size_t>(p - begin);
  return true;
}

inline bool token_is(const char *begin, std::size_t len, const char *word) {
  if (std::strlen(word) != len)
    return false;
  for (std::size_t i = 0; i < len; ++i) {
    if (std::tolower(static_cast<unsigned char>(begin[i])) != word[i])
      return false;
  }
  return true;
}

inline int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

// A hex number of at most `max` (an optional 0x prefix allowed).
inline bool parse_hex(const char *begin, std::size_t len, unsigned long max, unsigned long &out) {
  if (len > 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
    begin += 2;
    len -= 2;
  }
  if (len == 0 || len > 8)
    return false;
  unsigned long value = 0;
  for (std::size_t i = 0; i < len; ++i) {
    const int d = hex_digit(begin[i]);
    if (d < 0)
      return false;
    value = value * 16 + static_cast<unsigned long>(d);
  }
  if (value > max)
    return false;
  out = value;
  return true;
}

inline bool parse_dec(const char *begin, std::size_t len, unsigned long max, unsigned long &out) {
  if (len == 0 || len > 5)
    return false;
  unsigned long value = 0;
  for (std::size_t i = 0; i < len; ++i) {
    if (begin[i] < '0' || begin[i] > '9')
      return false;
    value = value * 10 + static_cast<unsigned long>(begin[i] - '0');
  }
  if (value > max)
    return false;
  out = value;
  return true;
}

}  // namespace raw_op_detail

// Parses a spec (see the top of this file). On failure returns false and
// points `error` at a static message naming the field. `read_max` caps the
// length a read may ask for.
inline bool parse_raw_spec(const char *text, RawSpec &out, const char *&error, uint8_t read_max) {
  using namespace raw_op_detail;
  out = RawSpec{};
  error = nullptr;
  if (text == nullptr) {
    error = "empty spec";
    return false;
  }
  const char *p = text;
  const char *tok = nullptr;
  std::size_t len = 0;

  if (!next_token(p, tok, len)) {
    error = "empty spec";
    return false;
  }
  if (token_is(tok, len, "p300") || token_is(tok, len, "vs2")) {
    out.layer = RawLayer::P300;
  } else if (token_is(tok, len, "kw") || token_is(tok, len, "vs1")) {
    out.layer = RawLayer::KW;
  } else if (token_is(tok, len, "gwg")) {
    out.layer = RawLayer::GWG;
  } else {
    error = "layer: p300, kw or gwg";
    return false;
  }

  if (!next_token(p, tok, len) || !(token_is(tok, len, "r") || token_is(tok, len, "w"))) {
    error = "direction: r or w";
    return false;
  }
  out.has_payload = token_is(tok, len, "w");

  unsigned long value = 0;
  const unsigned long code_max = out.layer == RawLayer::P300 ? 0x1F : 0xFF;
  if (!next_token(p, tok, len) || !parse_hex(tok, len, code_max, value) || value == 0) {
    error = out.layer == RawLayer::P300 ? "code: hex 01..1F" : "code: hex 01..FF";
    return false;
  }
  out.code = static_cast<uint8_t>(value);

  const unsigned long address_max = out.layer == RawLayer::GWG ? 0xFF : 0xFFFF;
  if (!next_token(p, tok, len) || !parse_hex(tok, len, address_max, value)) {
    error = out.layer == RawLayer::GWG ? "address: hex 00..FF" : "address: hex 0000..FFFF";
    return false;
  }
  out.address = static_cast<uint16_t>(value);

  if (!out.has_payload) {
    if (!next_token(p, tok, len) || !parse_dec(tok, len, read_max, value)) {
      error = "length: decimal, at most the read cap";
      return false;
    }
    // A remote procedure call sends its parameter as payload (w); without
    // one it asks for nothing. Nothing else may ask for 0 bytes.
    const bool call = out.layer == RawLayer::P300 && out.code == 0x07;
    if (call != (value == 0)) {
      error = call ? "length: 0 for a call without parameter (07); send a parameter with w" : "length: at least 1";
      return false;
    }
    out.length = static_cast<uint8_t>(value);
  } else {
    std::size_t n = 0;
    while (next_token(p, tok, len)) {
      if (len > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        tok += 2;
        len -= 2;
      }
      if (len % 2 != 0) {
        error = "bytes: hex, two digits each";
        return false;
      }
      for (std::size_t i = 0; i < len; i += 2) {
        const int hi = hex_digit(tok[i]);
        const int lo = hex_digit(tok[i + 1]);
        if (hi < 0 || lo < 0) {
          error = "bytes: hex, two digits each";
          return false;
        }
        if (n == RAW_OP_PAYLOAD_MAX) {
          error = "bytes: at most 32";
          return false;
        }
        out.payload[n++] = static_cast<uint8_t>(hi * 16 + lo);
      }
    }
    if (n == 0) {
      error = "bytes: at least one";
      return false;
    }
    out.length = static_cast<uint8_t>(n);
    return true;
  }

  if (next_token(p, tok, len)) {
    error = "trailing field after the length";
    return false;
  }
  return true;
}

}  // namespace esphome::vitohome
