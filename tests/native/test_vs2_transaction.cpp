#include <algorithm>
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "fixture_vectors.h"
#include "optolink/optolink.h"  // vendored in-tree engine

// The harness lives at global scope, so alias the engine namespace to keep the
// optolink:: spellings used by the component. The engine is a pure byte-mover:
// its response callback delivers (data, length, address), and read/write take
// (address, length) / (address, data, length) primitives -- no Datapoint. The
// payload pointer is engine-owned storage valid only during the callback, so
// copy it out there.
namespace optolink = esphome::vitohome::optolink;

template <class Pump>
static void handshake(FakeOptolink& io, Pump pump) {
  pump(3);
  io.feed({0x05});  // EOT -> ENQ
  pump(3);
  io.feed({0x06});  // SYNC -> ACK  => IDLE
  pump(3);
  io.clear_written();
}

static bool run_vector(const TransactionVector& tv) {
  FakeOptolink io;
  optolink::OptolinkEngine<optolink::P300> vito(&io);
  std::vector<uint8_t> got_payload;
  bool got_resp = false;
  vito.onResponse([&](const uint8_t* data, uint8_t length, uint16_t /*address*/) {
    // data is nullptr for write-ack responses (by design); only read the
    // payload for read responses. This is the correct consumer guard.
    if (data) got_payload.assign(data, data + length);
    got_resp = true;
  });
  vito.begin();
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i) vito.loop();
  };
  handshake(io, pump);

  if (tv.kind == Kind::WRITE)
    vito.write(tv.address, tv.write_data.data(), static_cast<uint8_t>(tv.write_data.size()));
  else
    vito.read(tv.address, tv.read_len);

  pump(8);  // engine emits request, lands in SEND_ACK
  for (const auto& chunk : tv.device_chunks) {
    io.feed(chunk);
    pump(6);
  }

  std::vector<uint8_t> expect = tv.request;
  expect.push_back(0x06);  // engine's response-ACK
  bool wire_ok = (io.written() == expect);
  // READ: payload must match captured bytes. WRITE: a bare ack just needs to complete.
  bool payload_ok = got_resp && (tv.kind == Kind::WRITE || got_payload == tv.payload);

  std::printf("  %-22s %-5s addr=0x%04X frag=%d  wire=%-4s resp=%-4s\n", tv.name,
              tv.kind == Kind::WRITE ? "WRITE" : "READ", tv.address, tv.fragmented, wire_ok ? "ok" : "FAIL",
              payload_ok ? "ok" : "FAIL");
  return wire_ok && payload_ok;
}

int main() {
  std::printf("VS2 transaction harness (golden-master vectors from live vitohome captures)\n");
  int fails = 0;
  for (const auto& tv : transaction_vectors())
    if (!run_vector(tv)) ++fails;
  std::printf("\n%zu vectors, %d failure(s)\n", transaction_vectors().size(), fails);
  return fails == 0 ? 0 : 1;
}
