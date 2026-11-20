// The codecs (optolink_codec_*.h) against the packet classes they replace,
// in the frozen reference copy (reference/): for every input the old class
// takes or refuses, the codec does the same and builds the same bytes
// (design-optolink-layers.md, section 9, step 3).
//
//   KW    kw_encode() against PacketVS1::createTelegram(), frame_encode()
//         against PacketVS1::createFrame()
//   P300  p300_encode() against PacketVS2::createPacket(REQUEST, ...) with its
//         41 and checksum, P300Parser against ParserVS2 byte by byte
//   GWG   gwg_encode(), gwg_encode_mode() and gwg_eeprom_pair() against
//         PacketGWG's createTelegram(), createPacket() and createEepromPair()
//         under both EOT positions, the 01 the packet puts first aside;
//         frame_encode() against createFrame()
//
// Every TYPE byte or function code, every length 0..255, boundary addresses,
// with and without a payload. The parser runs on generated streams of frames,
// noise and broken frames, every byte's result and every completed frame
// compared.

#include <cstdio>
#include <cstring>
#include <vector>

#include "optolink_codec_gwg.h"
#include "optolink_codec_kw.h"
#include "optolink_codec_p300.h"
#include "reference/optolink/optolink.h"

namespace live = esphome::vitohome::optolink;
namespace ref = esphome::vitohome::optolink_ref;

namespace {

int g_fail = 0;
long g_cases = 0;

void check(bool ok, const char *what) {
  std::printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// The first few differences, for the log.
int g_shown = 0;
void differ(const char *what, unsigned a, unsigned b, unsigned c) {
  if (g_shown++ < 8)
    std::printf("    %s: 0x%02X 0x%04X %u\n", what, a, b, c);
}

constexpr uint16_t kAddresses[] = {0x0000, 0x0001, 0x00F8, 0x00FF, 0x0100, 0x5525, 0x7FFF, 0xFFFF};

uint8_t g_payload[256];

bool kw() {
  bool ok = true;
  live::Telegram t{};
  for (unsigned type = 0; type < 256; ++type) {
    for (uint16_t address : kAddresses) {
      for (unsigned length = 0; length < 256; ++length) {
        for (const uint8_t *payload :
             {static_cast<const uint8_t *>(nullptr), static_cast<const uint8_t *>(g_payload)}) {
          ref::PacketVS1 p;
          const bool a = p.createTelegram(static_cast<uint8_t>(type), address, static_cast<uint8_t>(length), payload);
          const bool b = live::kw_encode(static_cast<uint8_t>(type), address, static_cast<uint8_t>(length), payload, t);
          ++g_cases;
          bool same = a == b;
          if (same && a) {
            same = p.length() == t.length && p.hasPayload() == t.has_payload;
            for (uint8_t i = 0; same && i < t.length; ++i)
              same = p[i] == t.bytes[i];
          }
          if (!same) {
            ok = false;
            differ("kw", type, address, length);
          }
        }
      }
    }
  }
  return ok;
}

bool frames(uint8_t max, bool gwg) {
  bool ok = true;
  live::Telegram t{};
  for (unsigned length = 0; length < 256; ++length) {
    for (const uint8_t *bytes : {static_cast<const uint8_t *>(nullptr), static_cast<const uint8_t *>(g_payload)}) {
      bool a;
      std::vector<uint8_t> got;
      if (gwg) {
        ref::PacketGWG p;
        a = p.createFrame(bytes, static_cast<uint8_t>(length));
        for (uint8_t i = 0; a && i < p.length(); ++i)
          got.push_back(p[i]);
        a = a && !p.hasPayload();
      } else {
        ref::PacketVS1 p;
        a = p.createFrame(bytes, static_cast<uint8_t>(length));
        for (uint8_t i = 0; a && i < p.length(); ++i)
          got.push_back(p[i]);
        a = a && !p.hasPayload();
      }
      const bool b = live::frame_encode(bytes, static_cast<uint8_t>(length), max, t);
      ++g_cases;
      bool same = a == b;
      if (same && a)
        same = got == std::vector<uint8_t>(t.bytes, t.bytes + t.length) && !t.has_payload;
      if (!same) {
        ok = false;
        differ(gwg ? "gwg frame" : "kw frame", 0, 0, length);
      }
    }
  }
  return ok;
}

bool p300() {
  bool ok = true;
  live::Telegram t{};
  for (unsigned function = 0; function < 256; ++function) {
    for (uint16_t address : kAddresses) {
      for (unsigned length = 0; length < 256; ++length) {
        for (const uint8_t *payload :
             {static_cast<const uint8_t *>(nullptr), static_cast<const uint8_t *>(g_payload)}) {
          ref::PacketVS2 p;
          const bool a = p.createPacket(ref::PacketType::REQUEST, static_cast<ref::FunctionCode>(function), 0, address,
                                        static_cast<uint8_t>(length), payload);
          const bool b =
              live::p300_encode(static_cast<uint8_t>(function), address, static_cast<uint8_t>(length), payload, t);
          ++g_cases;
          bool same = a == b;
          if (same && a) {
            std::vector<uint8_t> want{0x41};
            for (uint8_t i = 0; i < p.length(); ++i)
              want.push_back(p[i]);
            want.push_back(p.checksum());
            same = want == std::vector<uint8_t>(t.bytes, t.bytes + t.length) &&
                   live::p300_function(t) == (function & 0x1F) && live::p300_address(t) == address;
          }
          if (!same) {
            ok = false;
            differ("p300", function, address, length);
          }
        }
      }
    }
  }
  return ok;
}

// The packet's bytes after its leading 01 against the codec's.
bool same_gwg(ref::PacketGWG &p, bool a, const live::Telegram &t, bool b) {
  if (a != b)
    return false;
  if (!a)
    return true;
  if (p.length() != t.length + 1u || p[0] != 0x01 || p.hasPayload() != t.has_payload)
    return false;
  for (uint8_t i = 0; i < t.length; ++i) {
    if (p[1u + i] != t.bytes[i])
      return false;
  }
  return true;
}

bool gwg() {
  bool ok = true;
  live::Telegram t{};
  for (bool eot_first : {false, true}) {
    for (unsigned type = 0; type < 256; ++type) {
      for (uint16_t address : kAddresses) {
        for (unsigned length = 0; length < 256; ++length) {
          for (const uint8_t *payload :
               {static_cast<const uint8_t *>(nullptr), static_cast<const uint8_t *>(g_payload)}) {
            const auto ty = static_cast<uint8_t>(type);
            const auto len = static_cast<uint8_t>(length);
            ref::PacketGWG p;
            p.setEotBeforePayload(eot_first);
            bool a = p.createTelegram(ty, address, len, payload);
            bool b = live::gwg_encode(ty, address, len, payload, eot_first, t);
            ++g_cases;
            if (!same_gwg(p, a, t, b)) {
              ok = false;
              differ(eot_first ? "gwg telegram, EOT first" : "gwg telegram", type, address, length);
            }
            ref::PacketGWG q;
            q.setEotBeforePayload(eot_first);
            a = q.createPacket(ty, address, len, payload);
            b = live::gwg_encode_mode(ty, address, len, payload, eot_first, t);
            ++g_cases;
            if (!same_gwg(q, a, t, b)) {
              ok = false;
              differ(eot_first ? "gwg packet, EOT first" : "gwg packet", type, address, length);
            }
          }
        }
      }
    }
  }
  for (uint16_t address : kAddresses) {
    for (unsigned b0 = 0; b0 < 256; b0 += 17) {
      for (unsigned b1 = 0; b1 < 256; b1 += 13) {
        ref::PacketGWG p;
        const bool a = p.createEepromPair(address, static_cast<uint8_t>(b0), static_cast<uint8_t>(b1));
        const bool b = live::gwg_eeprom_pair(address, static_cast<uint8_t>(b0), static_cast<uint8_t>(b1), t);
        ++g_cases;
        if (!same_gwg(p, a, t, b)) {
          ok = false;
          differ("gwg eeprom pair", b0, address, b1);
        }
      }
    }
  }
  return ok;
}

uint32_t g_rng = 1;
// How the parser ended, per P300Parse, so a run shows it reached every result.
long g_parse_results[4] = {};
uint8_t next_byte() {
  g_rng = g_rng * 1103515245u + 12345u;
  return static_cast<uint8_t>(g_rng >> 16);
}

// A stream of P300 frames: well-formed answers of every kind, frames with a
// wrong checksum, a wrong DL or a bad length, and noise between them.
std::vector<uint8_t> stream(std::size_t frames) {
  std::vector<uint8_t> s;
  for (std::size_t f = 0; f < frames; ++f) {
    const uint8_t kind = next_byte() % 8;
    if (kind == 0) {
      for (uint8_t n = next_byte() % 6; n != 0; --n)
        s.push_back(next_byte());
      continue;
    }
    static constexpr uint8_t kTypes[] = {0x00, 0x01, 0x01, 0x01, 0x03, 0x02, 0x04};
    static constexpr uint8_t kFunctions[] = {0x01, 0x02, 0x07, 0x05, 0x01, 0x21, 0x0B};
    const uint8_t type = kTypes[next_byte() % sizeof(kTypes)];
    const uint8_t flags = kFunctions[next_byte() % sizeof(kFunctions)];
    const uint8_t payload = next_byte() % 70;
    uint8_t dl = payload;
    if (kind == 1)
      dl = next_byte();
    uint8_t ll = static_cast<uint8_t>(5 + payload);
    if (kind == 2)
      ll = next_byte();
    std::vector<uint8_t> body{ll, type, flags, next_byte(), next_byte(), dl};
    for (uint8_t i = 0; i < payload; ++i)
      body.push_back(next_byte());
    uint8_t cs = 0;
    for (uint8_t b : body)
      cs = static_cast<uint8_t>(cs + b);
    if (kind == 3)
      cs = static_cast<uint8_t>(cs + 1);
    s.push_back(0x41);
    s.insert(s.end(), body.begin(), body.end());
    s.push_back(cs);
  }
  return s;
}

bool parser() {
  bool ok = true;
  for (uint8_t expected : {uint8_t{0}, uint8_t{0x05}, uint8_t{0x0B}, uint8_t{0x21}}) {
    for (int run = 0; run < 40; ++run) {
      ref::internals::ParserVS2 a;
      live::P300Parser b;
      a.expectFunction(expected);
      b.expect_function(expected);
      const std::vector<uint8_t> s = stream(200);
      for (std::size_t i = 0; i < s.size(); ++i) {
        if (i % 997 == 996) {
          a.reset();
          b.reset();
        }
        const auto ra = static_cast<int>(a.parse(s[i]));
        const auto rb = static_cast<int>(b.parse(s[i]));
        ++g_cases;
        ++g_parse_results[rb];
        bool same = ra == rb;
        if (same && rb == static_cast<int>(live::P300Parse::COMPLETE)) {
          const auto &pa = a.packet();
          const auto &pb = b.frame();
          same = pa.length() == pb.length() &&
                 static_cast<uint8_t>(pa.packetType()) == static_cast<uint8_t>(pb.packet_type()) &&
                 static_cast<uint8_t>(pa.functionCode()) == pb.function_code() && pa.address() == pb.address() &&
                 pa.dataLength() == pb.data_length() && pa.payloadLength() == pb.payload_length() &&
                 (pa.data() == nullptr) == (pb.data() == nullptr) && pa.checksum() == pb.checksum() &&
                 std::memcmp(pa.payload(), pb.payload(), pb.payload_length()) == 0;
        }
        if (!same) {
          ok = false;
          differ("parser", s[i], static_cast<unsigned>(i), static_cast<unsigned>(rb));
          break;
        }
      }
    }
  }
  return ok;
}

}  // namespace

int main() {
  for (std::size_t i = 0; i < sizeof(g_payload); ++i)
    g_payload[i] = static_cast<uint8_t>(i * 7 + 3);
  std::printf("== the codecs against the packet classes they replace ==\n");
  check(kw(), "KW: kw_encode() builds and refuses as PacketVS1::createTelegram()");
  check(frames(live::KW_FRAME_MAX, false), "KW: frame_encode() as PacketVS1::createFrame()");
  check(p300(), "P300: p300_encode() as PacketVS2::createPacket() with 41 and checksum");
  check(parser(), "P300: P300Parser as ParserVS2, byte by byte");
  std::printf("    frames completed %ld, checksum errors %ld, malformed %ld\n", g_parse_results[1], g_parse_results[2],
              g_parse_results[3]);
  check(gwg(), "GWG: gwg_encode(), gwg_encode_mode(), gwg_eeprom_pair() as PacketGWG");
  check(frames(live::GWG_FRAME_MAX, true), "GWG: frame_encode() as PacketGWG::createFrame()");
  std::printf("codec equality: %ld cases, %d failure(s)\n", g_cases, g_fail);
  return g_fail == 0 ? 0 : 1;
}
