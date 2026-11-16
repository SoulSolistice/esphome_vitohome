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
//   <layer> f <bytes...> [/ <length>]        an open frame
//
//   layer    p300 (vs2), kw (vs1), gwg
//   code     hex: the P300 function code (1..1F, the low five bits of the
//            flags byte), the KW type byte, the GWG TYPE byte
//   address  hex, 0..FFFF (GWG: 0..FF)
//   length   decimal, the bytes asked for (P300 code 07 may ask for 0)
//   bytes    hex, as separate bytes ("00 0A") or runs ("000A")
//
// r and w stay inside the layer's protocol: the engine builds the telegram
// and checks the answer by that protocol's rules. f is outside every
// protocol: nothing is added to the bytes and nothing in the answer is
// checked; only the link's timing applies.
//
// An open frame goes out exactly as given and is answered with `length`
// bytes, or without `/ <length>` with whatever arrives until the line is
// quiet. Its layer must be the link's protocol. On KW and GWG it answers the
// device's ENQ, so a leading 01 is part of it; on P300 it goes out inside the
// session, the 41 and the checksum included, and the answer is raw, the ACK
// too. Bytes between `[` and `]` are followed by their CRC-16/KERMIT, low byte
// first: the KM-bus telegram check (openv wiki, KM-Bus). Bytes between `{`
// and `}` are followed by their sum modulo 256: the P300 checksum, over the
// bytes after the 41. At most 32 bytes, the check bytes included.
//
//   p300 r 05 1000 16      EEPROM read, 16 bytes at 0x1000
//   kw r 7B 0800 2         PROZESS_READ (over P300: the KW detour)
//   gwg r 31 05 1          an unnamed GWG TYPE byte
//   p300 w 02 2306 25      Virtual_WRITE of one byte
//   gwg f 01 CB 6F 01 04 / 1             a physical read, written out
//   gwg f 01 43 [01 00 33 0A 01 01 F8 04] 04
//                          a KM-bus telegram with its CRC appended
//   kw f 01 F7 00 F8 02 / 2              a KW read, written out
//   p300 f 41 {05 00 01 00 F8 02}        a P300 read with its checksum

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
// write, KW GFA_WRITE / PROZESS_WRITE, a GWG EEPROM pair sent as given), a
// payload on a read code.
enum class RawClass : uint8_t { READ, PROBE, WRITE, UNSAFE };

inline constexpr std::size_t RAW_OP_PAYLOAD_MAX = 32;

struct RawSpec {
  RawLayer layer;
  uint8_t code;
  uint16_t address;
  // The bytes asked for (r, f; 0 for f: until the line is quiet), or the
  // payload's length (w).
  uint8_t length;
  bool has_payload;
  // An open frame (f): `payload` holds its `frame_len` bytes; code and address
  // are 0.
  bool frame;
  uint8_t frame_len;
  uint8_t payload[RAW_OP_PAYLOAD_MAX];
};

// CRC-16/KERMIT (poly 0x1021 reflected, init 0): the KM-bus telegram check.
inline uint16_t km_crc16(const uint8_t *data, std::size_t len) {
  uint16_t crc = 0;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 1) != 0 ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
  }
  return crc;
}

// The P300 checksum: the sum of the bytes after the 41, modulo 256.
inline uint8_t p300_sum8(const uint8_t *data, std::size_t len) {
  uint8_t sum = 0;
  for (std::size_t i = 0; i < len; ++i)
    sum = static_cast<uint8_t>(sum + data[i]);
  return sum;
}

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

inline RawClass raw_op_class(RawLayer layer, uint8_t code, std::size_t payload_len) {
  const bool has_payload = payload_len != 0;
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
      // Two bytes on an EEPROM write are the cell's pair as given
      // (GWGEngine::telegram()), unchecked.
      if (code == optolink::gwgWriteTypeByte(optolink::GWGAccessMode::EEPROM) && payload_len == 2)
        return RawClass::UNSAFE;
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

// The rest of an open frame spec, after `<layer> f`.
inline bool parse_raw_frame(const char *p, RawSpec &out, const char *&error, uint8_t read_max) {
  using namespace raw_op_detail;
  const char *tok = nullptr;
  std::size_t len = 0;
  std::size_t n = 0;
  std::size_t group_from = 0;
  char group = 0;  // '[' or '{' while one is open
  bool answer = false;
  while (next_token(p, tok, len)) {
    if (len == 1 && tok[0] == '/') {
      unsigned long value = 0;
      if (!next_token(p, tok, len) || !parse_dec(tok, len, read_max, value)) {
        error = "length: decimal after /, at most the read cap";
        return false;
      }
      out.length = static_cast<uint8_t>(value);
      answer = true;
      break;
    }
    if (len != 0 && (tok[0] == '[' || tok[0] == '{')) {
      if (group != 0) {
        error = "bytes: a group inside [ ] or { }";
        return false;
      }
      group = tok[0];
      group_from = n;
      ++tok;
      --len;
    }
    const char close = len != 0 && (tok[len - 1] == ']' || tok[len - 1] == '}') ? tok[len - 1] : 0;
    if (close != 0)
      --len;
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
        error = "bytes: at most 32, the check bytes included";
        return false;
      }
      out.payload[n++] = static_cast<uint8_t>(hi * 16 + lo);
    }
    if (close != 0) {
      if (group != (close == ']' ? '[' : '{') || n == group_from) {
        error = "bytes: ] or } without its [ or {, or with nothing inside";
        return false;
      }
      const bool crc = group == '[';
      if (n + (crc ? 2 : 1) > RAW_OP_PAYLOAD_MAX) {
        error = "bytes: at most 32, the check bytes included";
        return false;
      }
      if (crc) {
        const uint16_t value = km_crc16(out.payload + group_from, n - group_from);
        out.payload[n++] = static_cast<uint8_t>(value & 0xFF);
        out.payload[n++] = static_cast<uint8_t>(value >> 8);
      } else {
        out.payload[n] = p300_sum8(out.payload + group_from, n - group_from);
        ++n;
      }
      group = 0;
    }
  }
  if (group != 0) {
    error = "bytes: [ or { never closed";
    return false;
  }
  if (n == 0) {
    error = "bytes: at least one";
    return false;
  }
  if (answer && next_token(p, tok, len)) {
    error = "trailing field after the length";
    return false;
  }
  out.frame = true;
  out.frame_len = static_cast<uint8_t>(n);
  return true;
}

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

  if (!next_token(p, tok, len) || !(token_is(tok, len, "r") || token_is(tok, len, "w") || token_is(tok, len, "f"))) {
    error = "direction: r, w or f";
    return false;
  }
  if (token_is(tok, len, "f"))
    return parse_raw_frame(p, out, error, read_max);
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
