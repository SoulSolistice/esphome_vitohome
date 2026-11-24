// Regression proof for the VS2 parser zero-payload out-of-bounds write.
//
// A frame whose payload-length byte is 0x00 and whose (functionCode,
// packetType) is a payload-bearing type used to enter the PAYLOAD step with
// _payloadLength == 0. The first payload byte post-decremented that uint8_t
// from 0 to 255, so the "== 0" completion check never fired: byte #1 landed
// at index 6 (still inside the buffer) and byte #2 wrote
// _packet[6 + dataLength() - _payloadLength] = _packet[6 + 0 - 255] -- an
// out-of-bounds write through std::array::operator[], reachable from garbled
// RX before the checksum is verified. Inherited verbatim from upstream
// VitoWiFi @ edc059a. The fix routes a zero-length
// payload straight to CHECKSUM.
//
// Two scenarios, matching the two pre-fix failure modes:
//   A) header + correct checksum: must COMPLETE cleanly with dataLength()==0.
//      Pre-fix, the checksum byte was consumed as phantom payload byte #1 and
//      the frame never completed -- caught by the assertions (no sanitizer
//      involvement; that byte still lands inside the buffer).
//   B) header + stray bytes: pre-fix, the SECOND stray byte performs the OOB
//      write, so under -fsanitize=address this run traps: an out-of-bounds
//      WRITE inside P300Parser::parse (the report class -- SEGV,
//      heap-use-after-free or heap-buffer-overflow -- depends on what the
//      stray index happens to land in). Post-fix the parser is waiting in
//      CHECKSUM: the first stray byte is a checksum mismatch (CS_ERROR +
//      reset) and the rest are ignored as invalid start bytes.
//
// Build (from tests/native): under -fsanitize=address,undefined, with
// -I../../components/vitohome. build_and_run.sh runs it.

#include <cstdio>

#include "optolink_codec_p300.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::P300Parse;

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// Header for a length-5 frame: start(0x41) len(0x05) type=RESPONSE(0x01)
// fc=READ(0x01) addr(0x08 0x00) payloadlen(0x00). length()-6 == 0 == b, so
// the mismatch guard passes; the fix must route this to CHECKSUM rather than
// PAYLOAD. Parsers are heap-allocated so ASan frames the packet buffer with
// redzones.
static const uint8_t kHeader[] = {0x41, 0x05, 0x01, 0x01, 0x08, 0x00, 0x00};

int main() {
  std::printf("== VS2 parser zero-payload frame (OOB regression) ==\n");

  // --- Scenario A: valid frame (header + checksum) completes cleanly. ---
  auto *parser = new optolink::P300Parser();
  P300Parse r = P300Parse::CONTINUE;
  for (uint8_t b : kHeader)
    r = parser->parse(b);

  // After the payload-length byte the parser must be waiting for the
  // CHECKSUM, not consuming phantom payload bytes.
  check(r == P300Parse::CONTINUE, "A: zero-payload header does not over-complete");

  // Feed the checksum: the sum of the stored bytes _packet[0..5] =
  // 0x05 + 0x01 + 0x01 + 0x08 + 0x00 + 0x00 = 0x0F.
  r = parser->parse(0x0F);
  check(r == P300Parse::COMPLETE, "A: completes on checksum (no phantom payload)");
  check(parser->frame().data_length() == 0, "A: surfaced payload length is 0");
  delete parser;

  // --- Scenario B: stray bytes after the zero-length payload. ---
  // Pre-fix this is where the OOB write happened: stray byte #1 landed at
  // index 6 (in bounds) and stray byte #2 at index 6 + 0 - 255 = -249 -- ASan
  // traps the out-of-bounds WRITE inside P300Parser::parse. Post-fix the parser
  // is in CHECKSUM: byte #1 is a checksum mismatch (CS_ERROR, reset to
  // STARTBYTE) and bytes #2/#3 are ignored as invalid start bytes.
  parser = new optolink::P300Parser();
  for (uint8_t b : kHeader)
    parser->parse(b);
  r = parser->parse(0xAA);  // pre-fix: phantom payload #1 | post-fix: CS mismatch
  check(r == P300Parse::CS_ERROR, "B: stray byte is a checksum error, not payload");
  r = parser->parse(0xBB);  // pre-fix: the OOB write (ASan SEGV) | post-fix: ignored
  check(r == P300Parse::CONTINUE, "B: parser reset, stray byte ignored");
  r = parser->parse(0xCC);
  check(r == P300Parse::CONTINUE, "B: still waiting for a real start byte");
  delete parser;

  std::printf("vs2 zero-payload proof: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
