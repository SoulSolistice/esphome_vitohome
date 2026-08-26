// Proves the two 2026-08-24 additions built on top of the ENQ-misread fix:
//
//   (1/2) GWGEngine::onTiming -- fires once per successful READ with the age
//         of the ENQ we answered and the send->response delta, never for
//         writes, never for failures. Motivated by the same hardware capture
//         as RESPONSE_TIMEOUT_MS: host-batched API log timestamps could not
//         resolve these numbers, so the instrument has to be device-side.
//
//   (3)   GWGAccessMode -- read() takes an optional access mode that selects
//         the TYPE byte per the openv wiki's Protokoll-GWG table. Case 1
//         below is the regression guard: the pre-existing 2-argument call
//         must still produce the exact PHYSICAL READ frame (0x01 CB addr len
//         04) byte-for-byte, since every existing config and every other
//         proof in this suite relies on that.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <chrono>  // NOLINT [build/c++11]
#include <cstdio>
#include <thread>  // NOLINT [build/c++11]
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

namespace {
int g_responses = 0;
uint16_t g_last_response_addr = 0;
int g_timing_calls = 0;
uint32_t g_last_enq_age = 0;
uint32_t g_last_send_to_response = 0;
uint16_t g_last_timing_addr = 0;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *, uint8_t, uint16_t address) {
    g_responses++;
    g_last_response_addr = address;
  });
  adapter.onError([](optolink::OptolinkResult, uint16_t) {});
  adapter.onTiming([](uint32_t enq_age_ms, uint32_t send_to_response_ms, uint16_t address) {
    g_timing_calls++;
    g_last_enq_age = enq_age_ms;
    g_last_send_to_response = send_to_response_ms;
    g_last_timing_addr = address;
  });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- (3) regression guard: default (2-arg) read() is byte-identical -------
  // to PHYSICAL READ. Every existing call site (raw scan, entity poll lane,
  // every other proof in this suite) calls read(address, length) with no 3rd
  // argument; if this frame ever drifted from 0x01 CB addr len 04, every one
  // of those would silently start talking to the wrong access mode.
  check(adapter.read(0x006F, 1), "read(addr, len) [2-arg] accepted");
  uart.feed({0x05});
  adapter.loop();
  const std::vector<uint8_t> want_physical = {0x01, 0xCB, 0x6F, 0x01, 0x04};
  check(uart.written() == want_physical, "2-arg read == PHYSICAL READ frame (unchanged)");
  uart.feed({0x26});
  pump(adapter);
  check(g_responses == 1 && g_last_response_addr == 0x006F, "physical read completes normally");
  uart.clear_written();

  // --- (3) each access mode selects the documented TYPE byte ----------------
  // Table source: openv wiki, Protokoll-GWG (fetched 2026-08-24). Frame shape
  // is otherwise identical: 01 <TYPE> <ADDR> <LEN> 04.
  struct Case {
    optolink::GWGAccessMode mode;
    uint8_t expected_type;
    const char *name;
  };
  const Case cases[] = {
      {optolink::GWGAccessMode::PHYSICAL, 0xCB, "PHYSICAL -> 0xCB"},
      {optolink::GWGAccessMode::VIRTUAL, 0xC7, "VIRTUAL -> 0xC7"},
      {optolink::GWGAccessMode::EEPROM, 0xAE, "EEPROM -> 0xAE"},
      {optolink::GWGAccessMode::XRAM, 0xC5, "XRAM -> 0xC5"},
      {optolink::GWGAccessMode::PORT, 0x6E, "PORT -> 0x6E"},
      {optolink::GWGAccessMode::BE, 0x9E, "BE -> 0x9E"},
      {optolink::GWGAccessMode::KMBUS_RAM, 0x33, "KMBUS_RAM -> 0x33"},
      {optolink::GWGAccessMode::KMBUS_EEPROM, 0x43, "KMBUS_EEPROM -> 0x43"},
  };
  for (const auto &c : cases) {
    check(adapter.read(0x00F8, 2, c.mode), c.name);
    uart.feed({0x05});
    adapter.loop();
    const std::vector<uint8_t> want = {0x01, c.expected_type, 0xF8, 0x02, 0x04};
    check(uart.written() == want, "  frame matches table");
    uart.feed({0x20, 0x53});  // dannerph's tested virtual-0xF8 payload bytes
    pump(adapter);
    uart.clear_written();
  }

  // --- (3) PacketGWG still rejects unknown TYPE bytes ------------------------
  // Direct test of the tightened check in packet_gwg.cpp: it now accepts 8
  // read types instead of 1, but must still reject everything else (an
  // arbitrary byte that is neither a known read type nor WRITE).
  {
    optolink::PacketGWG pkt;
    check(!pkt.createPacket(0x99, 0xF8, 2, nullptr), "unknown TYPE byte (0x99) still rejected");
    const uint8_t write_byte = 0x01;
    check(pkt.createPacket(optolink::PacketGWGType.WRITE, 0xF8, 1, &write_byte), "WRITE (0xC8) still accepted");
  }

  // --- (1/2) timing instrument: fires on read, reports plausible deltas -----
  const int timing_calls_before_read = g_timing_calls;
  check(adapter.read(0x0070, 1), "timing: read accepted");
  uart.feed({0x05});
  adapter.loop();  // ENQ observed -> loop gap captured -> same-loop send
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  uart.feed({0x5F});
  pump(adapter);
  check(g_timing_calls == timing_calls_before_read + 1, "onTiming fired exactly once for this completed read");
  check(g_last_timing_addr == 0x0070, "onTiming reports the request address");
  // enq_age is the loop() gap preceding the iteration that consumed the ENQ.
  // Here the loops run back-to-back with no sleep in between, so it must be a
  // couple of ms at most. send->response should reflect the ~30ms sleep above.
  check(g_last_enq_age < 20, "enq age is small when loops run back-to-back");
  check(g_last_send_to_response >= 25 && g_last_send_to_response < 500, "send->response reflects the injected delay");

  // enq_age must actually MEASURE the host stall, not report a constant. This
  // is the assertion the instrument exists for: stall the host between two
  // loop() calls and the reported age has to move with it. (Measuring
  // ENQ-observed to frame-written instead would be identically 0 here -- the
  // same-loop fall-through puts both in one iteration, and _currentMillis is
  // sampled once per iteration.)
  const uint32_t enq_age_no_stall = g_last_enq_age;
  // Let the burst window lapse first (see GWGEngine::ENQ_VALIDITY_MS): with a
  // recent sync in hand the engine would send WITHOUT waiting for an ENQ, and
  // there would be no ENQ wait left to measure.
  std::printf("  (letting the burst window lapse, %u ms ...)\n",
              static_cast<unsigned>(optolink::GWGEngine::ENQ_VALIDITY_MS));
  std::this_thread::sleep_for(std::chrono::milliseconds(optolink::GWGEngine::ENQ_VALIDITY_MS + 150));
  check(adapter.read(0x0073, 1), "timing: read accepted (stalled host)");
  adapter.loop();  // establish a loop timestamp with nothing to read
  uart.feed({0x05});
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  adapter.loop();  // this iteration consumes the ENQ, 120 ms after the last one
  uart.feed({0x5F});
  pump(adapter);
  check(g_last_enq_age >= 100, "enq age tracks a real host stall");
  check(g_last_enq_age > enq_age_no_stall, "enq age is not a constant");

  // --- (1/2) timing instrument: must NOT fire for writes ---------------------
  const int timing_calls_before_write = g_timing_calls;
  const uint8_t write_payload = 0x01;
  check(adapter.write(0x0053, &write_payload, 1), "timing: write accepted");
  uart.feed({0x05});
  adapter.loop();
  uart.feed({0x06});  // write ack
  pump(adapter);
  check(g_timing_calls == timing_calls_before_write, "onTiming does not fire for writes");

  std::printf("gwg access-mode + timing proof: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
