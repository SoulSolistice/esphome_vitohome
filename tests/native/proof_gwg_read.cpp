// Proves GWG request/response COMPLETION against the vendored engine -- the
// path proof_gwg_poke.cpp never exercised (it only asserts the EOT-poke
// switch; no read has ever completed in a host test before this file).
//
// Guards the THIRD_PARTY.md #8 fix: upstream compared the received byte count
// against the REQUEST frame length (5 for a read), so a GWG read of any
// length != 5 could never complete. The correct completion is the datapoint's
// own length for a read (source-confirmed: vcontrold GWG getaddr = "SEND 01 CB
// $addr $hexlen 04; RECV $len") and one ack byte for a write (model-derived,
// KW-family convention; vcontrold's GWG setaddr entry is a stub).
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <cstdio>
#include <cstring>
#include <vector>

#include "fake_optolink.h"
#include "protocol_adapter.h"

using namespace esphome::vitohome;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_len = 0;
uint8_t g_last_payload[8] = {0};
uint16_t g_last_addr = 0;
}  // namespace

static void pump(ProtocolAdapter& a, int n = 6) {
  for (int i = 0; i < n; ++i) a.loop();
}

int main() {
  FakeOptolink uart;
  ProtocolAdapter adapter(&uart);  // GWGEngine under -DVITOHOME_PROTOCOL_GWG
  adapter.on_response([](const ResponseView& r, uint16_t) {
    g_responses++;
    g_last_len = r.data_length;
    g_last_addr = r.address;
    if (r.data != nullptr && r.data_length <= sizeof(g_last_payload)) {
      std::memcpy(g_last_payload, r.data, r.data_length);
    }
  });
  adapter.on_error([](optolink::OptolinkResult, uint16_t) { g_errors++; });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char* what) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
  };

  // --- READ of a 2-byte datapoint (upstream waited for 5 bytes here) --------
  optolink::Datapoint rd("gwg_read2", 0x0055, 2, optolink::noconv);
  check(adapter.read(rd), "read accepted");
  uart.feed({0x05});  // device ENQ -> engine syncs and sends the request
  pump(adapter);
  const std::vector<uint8_t> want_read = {0x01, 0xCB, 0x55, 0x02, 0x04};
  check(uart.written() == want_read, "read frame = 01 CB 55 02 04");
  uart.clear_written();
  uart.feed({0xAA, 0xBB});  // exactly len data bytes -- the vcontrold RECV $len
  pump(adapter);
  check(g_responses == 1 && g_errors == 0, "read completed on len bytes");
  check(g_last_len == 2 && g_last_payload[0] == 0xAA && g_last_payload[1] == 0xBB, "payload = AA BB, length 2");
  check(g_last_addr == 0x0055, "ResponseView.address = 0x0055");
  check(!adapter.is_busy(), "engine idle after read (not one-shot)");

  // --- WRITE of a 1-byte datapoint (ack = one byte, KW-family convention) ---
  optolink::Datapoint wr("gwg_write1", 0x0060, 1, optolink::noconv);
  const uint8_t data[1] = {0x07};
  check(adapter.write(wr, data, 1), "write accepted");
  uart.feed({0x05});  // device ENQ
  pump(adapter);
  const std::vector<uint8_t> want_write = {0x01, 0xC8, 0x60, 0x01, 0x07, 0x04};
  check(uart.written() == want_write, "write frame = 01 C8 60 01 07 04");
  uart.feed({0x00});  // single ack byte
  pump(adapter);
  check(g_responses == 2 && g_errors == 0, "write completed on 1 ack byte");
  check(!adapter.is_busy(), "engine idle after write");

  std::printf("gwg read/write completion: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
