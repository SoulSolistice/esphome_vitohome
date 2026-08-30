// Fuzz target + deterministic replay runner for ParserVS2.
//
// WHY THIS FILE EXISTS. ParserVS2::parse() is a byte-at-a-time state machine
// fed straight from the UART, i.e. from a noisy optical link where garbled RX
// is the ordinary case rather than an attack. It is also the only place in
// this codebase with a known history of memory-safety defects: the
// zero-payload out-of-bounds write (THIRD_PARTY.md #12) and, next door in the
// packet it fills, the RESPONSE payload guards (#16). Both were found by a
// person hand-writing the one input that trips them, and both are now pinned
// by targeted proofs. A coverage-guided fuzzer covers the inputs nobody
// thought to write down.
//
// TWO BUILDS, ONE SOURCE.
//
//   replay (CI)     g++ ... fuzz_parser_vs2.cpp -> walks the committed seed
//                   corpus in fuzz_corpus_vs2.h plus any files named on the
//                   command line. Deterministic, sub-second, needs no clang.
//                   This is the gate.
//   libFuzzer       clang -fsanitize=fuzzer,address,undefined
//                   -DVITOHOME_FUZZ_LIBFUZZER -> the open-ended search. Run by
//                   a person via fuzz.sh, not by CI: an unbounded search does
//                   not belong on a push, and a campaign that finds nothing is
//                   not evidence worth a CI minute.
//
// Splitting it this way is what makes a finding permanent. The fuzzer's job is
// to discover an input; the corpus header's job is to keep executing it
// forever afterwards. Minimise anything a campaign turns up, add it to
// fuzz_corpus_vs2.h with a note on what it broke, and CI carries it from then
// on.
//
// WHAT IS BEING CHECKED. Memory safety comes free from the sanitizers -- that
// is most of the value, and it is why this must never be built without them.
// On top of that the harness asserts four properties the parser is supposed to
// have, because a fuzzer with no oracle finds only crashes, and the defects
// this project actually fears are silent-wrong-value ones (design_notes.md
// §1a). See the checks in feed_and_check() for what each one is and why it is
// scoped the way it is.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fuzz_corpus_vs2.h"
#include "optolink/protocol/vs2/packet_vs2.h"
#include "optolink/protocol/vs2/parser_vs2.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::PacketVS2;
using optolink::PacketType;
using optolink::FunctionCode;
using optolink::internals::ParserResult;
using optolink::internals::ParserVS2;

namespace {

#ifndef VITOHOME_FUZZ_LIBFUZZER
// Only the replay runner counts failures; the libFuzzer build aborts on the
// first one, so the counter would be an unused variable there (-Werror).
int g_fail = 0;
#endif
const char *g_case = "(fuzz)";

// In libFuzzer mode a violated oracle must abort so the driver records a crash
// and writes the reproducer out; in replay mode it is a counted failure so the
// runner can report every broken seed in one go rather than dying on the first.
void oracle(bool ok, const char *what) {
  if (ok)
    return;
  std::fprintf(stderr, "ORACLE VIOLATED [%s]: %s\n", g_case, what);
#ifdef VITOHOME_FUZZ_LIBFUZZER
  std::abort();
#else
  ++g_fail;
#endif
}

// The resynchronisation probe: a frame the parser must always accept, built
// so that a COMPLETE can only mean THIS frame was parsed.
//
// Its address (0xBEEF) and payload (0xDE 0xAD) appear in no seed and in no
// real datapoint, and that is load-bearing rather than decorative. The first
// version of this probe reused the 0x5525 capture frame, and it silently
// failed to catch a deliberately wedged parser: with the CHECKSUM step's reset
// removed, the parser sat in CHECKSUM comparing every incoming byte against
// the checksum of the frame still in its buffer -- and the probe's own last
// byte was that same checksum, because it was the same frame. COMPLETE came
// back, the oracle passed, and the wedge went unnoticed. A probe whose
// identity is checked (below) and which cannot be impersonated by stale state
// is the whole difference between this oracle working and merely looking like
// it does.
constexpr uint8_t kProbeFrame[] = {0x41, 0x07, 0x01, 0x01, 0xBE, 0xEF, 0x02, 0xDE, 0xAD, 0x43};
constexpr uint16_t kProbeAddress = 0xBEEF;
constexpr uint8_t kProbePayload[] = {0xDE, 0xAD};

// Enough 0x00 bytes to drain any state the fuzz input can leave the parser in.
// The worst case is PAYLOAD with the largest payload the length arithmetic
// admits (249 bytes) still outstanding: 249 to reach CHECKSUM, one more to be
// tested as the checksum and reset. 0x00 is the right filler because it is
// inert at every step -- an invalid start byte at STARTBYTE, and below the
// minimum at PACKETLENGTH -- so it can only ever drive the machine toward
// STARTBYTE, never build a new frame that swallows the probe that follows.
constexpr int kDrainBytes = 260;

void check_complete_frame(const PacketVS2 &p, uint8_t checksum_byte) {
  // (1) The parser completed on a checksum it agrees with. Near-tautological
  // against the implementation, but it is the property that must survive any
  // future rework of the CHECKSUM step: COMPLETE means verified, always.
  oracle(p.checksum() == checksum_byte, "COMPLETE returned on a checksum the packet disagrees with");

  // (2) and (3) are scoped to RESPONSE frames ON PURPOSE, and the scope is the
  // interesting part.
  //
  // VS2Engine::_receive() forwards a completed frame to the response callback
  // only when packetType() == RESPONSE; anything else goes to onError as
  // DEVICE_ERROR (THIRD_PARTY.md #9). That guard was added for correctness --
  // upstream published an ERROR frame's payload as if it were data -- but it
  // is also what keeps a consumer inside the packet buffer, and that second
  // job is undocumented anywhere else.
  //
  // The reason: the parser skips the payload-length validation entirely for
  // the two frame shapes that carry no inline payload, READ+REQUEST and
  // WRITE+RESPONSE, and just stores the byte. A READ+REQUEST frame can
  // therefore complete with dataLength() == 255 while only six bytes were ever
  // written, and data() is NOT null for it (only fc == WRITE returns null). A
  // caller doing data()[0 .. dataLength()) on that frame reads 261 bytes out
  // of a 256-byte buffer. Today nothing can: REQUEST never reaches a consumer.
  // Relax the RESPONSE guard and it does. kSeed_read_request_len255 is that
  // frame, which is why the assertions below are not made for it.
  if (p.packetType() != PacketType::RESPONSE)
    return;

  if (p.data() != nullptr) {
    // (2) A delivered payload lies wholly inside the packet buffer. The
    // payload starts at index 6, so this is the bound a consumer relies on.
    const std::size_t end = 6u + static_cast<std::size_t>(p.dataLength());
    oracle(end <= PacketVS2::kMaxFrame, "delivered payload extends past the packet buffer");

    // (3) ... and it is exactly the payload the frame length declares. This is
    // the structural invariant behind (2): dataLength() is only trustworthy
    // because the PAYLOADLENGTH step rejected any frame where the two
    // disagree. length() is uint8_t arithmetic (_buffer[0] + 1) and wraps to 0
    // at a length byte of 255 -- the trap packet_vs2.cpp documents -- so a
    // regression there surfaces here rather than as a quiet short read.
    oracle(static_cast<int>(p.dataLength()) == static_cast<int>(p.length()) - 6,
           "delivered dataLength() disagrees with the frame length");
  }
}

}  // namespace

// One fuzz input == one byte stream arriving on the UART. It is fed to a
// SINGLE parser without resetting between frames, which is how VS2Engine holds
// it: a long-lived parser that must resynchronise on its own after every
// garbled frame. That is where the interesting sequences live -- a truncated
// frame followed by a good one, a false start byte inside a payload, a
// checksum that happens to match.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  // Heap-allocated so ASan surrounds the packet buffer with redzones; a
  // stack or static parser would make an out-of-bounds index far more likely
  // to land in adjacent valid memory and go unreported.
  auto *parser = new ParserVS2();

  for (std::size_t i = 0; i < size; ++i) {
    const uint8_t b = data[i];
    if (parser->parse(b) == ParserResult::COMPLETE)
      check_complete_frame(parser->packet(), b);
  }

  // (4) Liveness: no byte sequence may permanently wedge the parser. Drain
  // whatever state the input left behind, then feed a frame that is valid by
  // construction and require it to complete. This is the oracle that catches
  // state corruption rather than memory corruption -- a parser stuck in a step
  // it can never leave would keep the link dead until the device reboots,
  // which is precisely the class of failure §5a records for VitoClock's phase
  // guard, and it would not trip a sanitizer.
  for (int i = 0; i < kDrainBytes; ++i) {
    const uint8_t z = 0x00;
    if (parser->parse(z) == ParserResult::COMPLETE)
      check_complete_frame(parser->packet(), z);
  }
  ParserResult r = ParserResult::CONTINUE;
  for (uint8_t b : kProbeFrame)
    r = parser->parse(b);
  oracle(r == ParserResult::COMPLETE, "parser did not resynchronise: a valid frame failed to complete");

  // Completing SOMETHING is not resynchronising. Check the packet is the probe
  // -- address, payload length and payload bytes -- so a stale frame finishing
  // on a coincidental checksum match cannot pass for a recovered parser.
  if (r == ParserResult::COMPLETE) {
    const PacketVS2 &p = parser->packet();
    bool is_probe = p.address() == kProbeAddress && p.dataLength() == sizeof(kProbePayload) && p.data() != nullptr;
    if (is_probe)
      is_probe = std::memcmp(p.data(), kProbePayload, sizeof(kProbePayload)) == 0;
    oracle(is_probe, "resynchronised into the wrong frame: COMPLETE did not surface the probe");
  }

  delete parser;
  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

namespace {

// Replay one file named on the command line. Used for a reproducer a campaign
// just produced, before it has been minimised and promoted into
// fuzz_corpus_vs2.h.
bool replay_file(const char *path) {
  std::FILE *f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "  cannot open %s\n", path);
    return false;
  }
  static uint8_t buf[64 * 1024];
  const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);
  g_case = path;
  const int before = g_fail;
  LLVMFuzzerTestOneInput(buf, n);
  std::printf("  %-40s %4zu bytes  %s\n", path, n, g_fail == before ? "ok" : "FAIL");
  return g_fail == before;
}

}  // namespace

int main(int argc, char **argv) {
  std::printf("== VS2 parser fuzz corpus (deterministic replay) ==\n");

  for (std::size_t i = 0; i < kFuzzSeedCount; ++i) {
    const FuzzSeed &s = kFuzzSeeds[i];
    g_case = s.name;
    const int before = g_fail;
    LLVMFuzzerTestOneInput(s.bytes, s.len);
    std::printf("  %-40s %4zu bytes  %s\n", s.name, s.len, g_fail == before ? "ok" : "FAIL");
  }

  for (int a = 1; a < argc; ++a)
    replay_file(argv[a]);

  std::printf("vs2 parser fuzz corpus: %zu seed(s), %d failure(s)\n", kFuzzSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
