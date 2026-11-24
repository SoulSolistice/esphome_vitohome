// The codecs (optolink_codec_*.h) against their recorded outputs
// (fixtures/codecs.txt, golden.h): per family, the number of cases and a hash
// over every case's outcome, which is accepted or refused and the bytes built.
// The recorded outputs are those of the packet classes the codecs replaced
// (docs/design_notes.md, section 3).
//
//   KW    kw_encode(), frame_encode()
//   P300  p300_encode() with its 41 and checksum, its function and address
//         read back; P300Parser byte by byte, every completed frame's fields
//   GWG   gwg_encode(), gwg_encode_mode() under both EOT positions,
//         gwg_eeprom_pair(), frame_encode()
//
// Every TYPE byte or function code, every length 0..255, boundary addresses,
// with and without a payload. The parser runs on generated streams of frames,
// noise and broken frames.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "golden.h"
#include "optolink_codec_gwg.h"
#include "optolink_codec_kw.h"
#include "optolink_codec_p300.h"

namespace live = esphome::vitohome::optolink;

namespace {

int g_fail = 0;
long g_cases = 0;
Golden *g_golden = nullptr;

// FNV-1a over a family's outcomes.
struct Digest {
  uint64_t hash = 0xcbf29ce484222325ull;
  long cases = 0;
  void add(uint8_t b) {
    hash ^= b;
    hash *= 0x100000001b3ull;
  }
  void add16(uint16_t v) {
    add(static_cast<uint8_t>(v >> 8));
    add(static_cast<uint8_t>(v));
  }
  // One encoding: refused, or accepted with its bytes and payload flag.
  void telegram(bool ok, const live::Telegram &t) {
    ++cases;
    add(ok ? 1 : 0);
    if (!ok)
      return;
    add(t.length);
    add(t.has_payload ? 1 : 0);
    for (uint8_t i = 0; i < t.length; ++i)
      add(t.bytes[i]);
  }
};

void check(const char *family, const Digest &d) {
  g_cases += d.cases;
  char hash[32];
  std::snprintf(hash, sizeof(hash), "fnv1a64 %016" PRIx64, d.hash);
  const std::string diff = g_golden->check(family, {"cases " + std::to_string(d.cases), hash});
  std::printf("  %-70s %s\n", family, diff.empty() ? "ok" : "FAIL");
  if (!diff.empty()) {
    std::printf("    %s\n", diff.c_str());
    ++g_fail;
  }
}

constexpr uint16_t kAddresses[] = {0x0000, 0x0001, 0x00F8, 0x00FF, 0x0100, 0x5525, 0x7FFF, 0xFFFF};

uint8_t g_payload[256];

const uint8_t *const kPayloads[] = {nullptr, g_payload};

Digest kw() {
  Digest d;
  live::Telegram t{};
  for (unsigned type = 0; type < 256; ++type) {
    for (uint16_t address : kAddresses) {
      for (unsigned length = 0; length < 256; ++length) {
        for (const uint8_t *payload : kPayloads) {
          const bool ok =
              live::kw_encode(static_cast<uint8_t>(type), address, static_cast<uint8_t>(length), payload, t);
          d.telegram(ok, t);
        }
      }
    }
  }
  return d;
}

Digest frames(uint8_t max) {
  Digest d;
  live::Telegram t{};
  for (unsigned length = 0; length < 256; ++length) {
    for (const uint8_t *bytes : kPayloads)
      d.telegram(live::frame_encode(bytes, static_cast<uint8_t>(length), max, t), t);
  }
  return d;
}

Digest p300() {
  Digest d;
  live::Telegram t{};
  for (unsigned function = 0; function < 256; ++function) {
    for (uint16_t address : kAddresses) {
      for (unsigned length = 0; length < 256; ++length) {
        for (const uint8_t *payload : kPayloads) {
          const bool ok =
              live::p300_encode(static_cast<uint8_t>(function), address, static_cast<uint8_t>(length), payload, t);
          d.telegram(ok, t);
          if (ok) {
            d.add(live::p300_function(t));
            d.add16(live::p300_address(t));
          }
        }
      }
    }
  }
  return d;
}

Digest gwg() {
  Digest d;
  live::Telegram t{};
  for (bool eot_first : {false, true}) {
    for (unsigned type = 0; type < 256; ++type) {
      for (uint16_t address : kAddresses) {
        for (unsigned length = 0; length < 256; ++length) {
          for (const uint8_t *payload : kPayloads) {
            const auto ty = static_cast<uint8_t>(type);
            const auto len = static_cast<uint8_t>(length);
            d.telegram(live::gwg_encode(ty, address, len, payload, eot_first, t), t);
            d.telegram(live::gwg_encode_mode(ty, address, len, payload, eot_first, t), t);
          }
        }
      }
    }
  }
  for (uint16_t address : kAddresses) {
    for (unsigned b0 = 0; b0 < 256; b0 += 17) {
      for (unsigned b1 = 0; b1 < 256; b1 += 13)
        d.telegram(live::gwg_eeprom_pair(address, static_cast<uint8_t>(b0), static_cast<uint8_t>(b1), t), t);
    }
  }
  return d;
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

Digest parser() {
  Digest d;
  for (uint8_t expected : {uint8_t{0}, uint8_t{0x05}, uint8_t{0x0B}, uint8_t{0x21}}) {
    for (int run = 0; run < 40; ++run) {
      live::P300Parser b;
      b.expect_function(expected);
      const std::vector<uint8_t> s = stream(200);
      for (std::size_t i = 0; i < s.size(); ++i) {
        if (i % 997 == 996)
          b.reset();
        const auto rb = static_cast<int>(b.parse(s[i]));
        ++d.cases;
        ++g_parse_results[rb];
        d.add(static_cast<uint8_t>(rb));
        if (rb == static_cast<int>(live::P300Parse::COMPLETE)) {
          const auto &pb = b.frame();
          d.add(pb.length());
          d.add(static_cast<uint8_t>(pb.packet_type()));
          d.add(pb.function_code());
          d.add16(pb.address());
          d.add(pb.data_length());
          d.add(pb.payload_length());
          d.add(pb.data() == nullptr ? 0 : 1);
          d.add(pb.checksum());
          for (uint8_t k = 0; k < pb.payload_length(); ++k)
            d.add(pb.payload()[k]);
        }
      }
    }
  }
  return d;
}

}  // namespace

int main() {
  for (std::size_t i = 0; i < sizeof(g_payload); ++i)
    g_payload[i] = static_cast<uint8_t>(i * 7 + 3);
  std::printf("== the codecs against their recorded outputs ==\n");
  Golden golden("codecs");
  g_golden = &golden;
  check("KW: kw_encode()", kw());
  check("KW: frame_encode()", frames(live::KW_FRAME_MAX));
  check("P300: p300_encode() with 41 and checksum", p300());
  check("P300: P300Parser, byte by byte", parser());
  std::printf("    frames completed %ld, checksum errors %ld, malformed %ld\n", g_parse_results[1], g_parse_results[2],
              g_parse_results[3]);
  check("GWG: gwg_encode(), gwg_encode_mode(), gwg_eeprom_pair()", gwg());
  check("GWG: frame_encode()", frames(live::GWG_FRAME_MAX));
  g_fail += golden.finish();
  std::printf("codec equality: %ld cases, %d failure(s)\n", g_cases, g_fail);
  return g_fail == 0 ? 0 : 1;
}
