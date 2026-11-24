// The links' trace for the recorder (LinkSink, design-optolink-layers.md
// 15.9), on a frozen clock: what each exchange reports, in order.
//   KW)   begin, a stray byte, a read on the ENQ, chained; silence and the
//         retry; the watchdog;
//   P300) the handshake, a read the device NACKs, a restart whose EOT it
//         refuses;
//   GWG)  a read on the ENQ, a burst-sent read met by the deadline and its
//         retry, bytes after a deadline, bursting off.
// Each state is named by the exchange's state_name(). That the trace
// changes no byte is the differential target's part: its live side runs
// with a sink attached (trace_sink.h).
//
// Built by build_and_run_protocols.sh.

#include <cstdio>
#include <string>
#include <vector>

#include "fake_optolink.h"
#include "gwg_calls.h"
#include "kw_calls.h"

namespace optolink = esphome::vitohome::optolink;
using Bytes = std::vector<uint8_t>;
using optolink::LinkFact;
using optolink::LinkTrace;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// The trace as text: a state by its name, a fact as `*name` or `*name=arg`.
struct Trace {
  const char *(*names)(uint8_t);
  std::vector<std::string> items;

  static void emit(void *context, LinkTrace trace, uint8_t value, uint16_t arg) {
    auto *self = static_cast<Trace *>(context);
    if (trace == LinkTrace::STATE) {
      self->items.emplace_back(self->names(value));
      return;
    }
    std::string item = std::string("*") + optolink::link_fact_name(static_cast<LinkFact>(value));
    if (arg != 0)
      item += "=" + std::to_string(arg);
    self->items.push_back(item);
  }
  optolink::LinkSink sink() { return optolink::LinkSink{&Trace::emit, this}; }
  // The items since the last take.
  std::string take() {
    std::string out;
    for (const auto &item : items)
      out += (out.empty() ? "" : " ") + item;
    items.clear();
    return out;
  }
};

template<class E> void pump(E &e, int n = 8) {
  for (int i = 0; i < n; ++i)
    e.loop();
}

template<class E> void advance(E &e, uint32_t ms) {
  optolink::optolink_test_clock_advance(ms);
  pump(e);
}

bool expect(Trace &trace, const char *want, const char *what) {
  const std::string got = trace.take();
  const bool ok = got == want;
  check(ok, what);
  if (!ok)
    std::printf("    got:  %s\n    want: %s\n", got.c_str(), want);
  return ok;
}

}  // namespace

int main() {
  optolink::optolink_test_clock_freeze();

  // --- KW -----------------------------------------------------------------
  {
    using Engine = optolink::KwExchange<FakeOptolink, kw_calls::kNoPause>;
    FakeOptolink io;
    Engine e(&io);
    Trace trace{&Engine::state_name, {}};
    e.on_result([](const optolink::Result &) {});
    e.set_sink(trace.sink());
    e.begin();
    expect(trace, "WAIT_ENQ", "KW: begin");
    io.feed({0x20});
    pump(e);
    expect(trace, "*drained=1", "KW: a stray byte while waiting for the ENQ");
    check(kw_calls::read(e, 0x00F8, 1), "KW: read queued");
    io.feed({0x05});
    pump(e);
    expect(trace, "ENQ_HELD SEND RECEIVE", "KW: the ENQ, 01 + telegram in one pass");
    io.feed({0x20});
    pump(e);
    expect(trace, "CHAINED", "KW: answered, chained");
    advance(e, 600);
    expect(trace, "WAIT_ENQ", "KW: the chain window passes");
    check(kw_calls::read(e, 0x00F9, 1), "KW: read queued");
    io.feed({0x05});
    pump(e);
    trace.take();
    advance(e, optolink::KW_LINK_PARAMS.answer_timeout_ms + 1);
    expect(trace, "WAIT_ENQ *retry", "KW: silence: back to the ENQ, the retry");
    advance(e, optolink::KW_WATCHDOG_MS + 1);
    const std::string late = trace.take();
    check(late.find("*watchdog") != std::string::npos, "KW: no ENQ: the watchdog");
  }

  // --- P300 ---------------------------------------------------------------
  {
    using Engine = optolink::P300Exchange<FakeOptolink>;
    FakeOptolink io;
    Engine e(&io);
    Trace trace{&Engine::state_name, {}};
    e.on_result([](const optolink::Result &) {});
    e.set_sink(trace.sink());
    e.begin();
    pump(e);
    io.feed({0x05});
    pump(e);
    io.feed({0x06});
    pump(e);
    expect(trace, "RESET RESET_ACK INIT INIT_ACK IDLE", "P300: the handshake");
    check(kw_calls::read(e, 0x00F8, 4), "P300: read queued");
    pump(e);
    expect(trace, "SEND_START SEND_PACKET SEND_CRC SEND_ACK", "P300: the telegram in one pass");
    io.feed({0x15});
    pump(e);
    expect(trace, "*nack IDLE", "P300: NACKed");
    e.begin();
    pump(e);
    expect(trace, "RESET RESET_ACK", "P300: a restart, its EOT");
    io.feed({0x15});
    pump(e);
    expect(trace, "*eot refused RESET", "P300: the EOT refused");
    advance(e, optolink::P300_QUIET_MS);
    expect(trace, "RESET_ACK", "P300: the EOT again after the quiet");
    advance(e, optolink::P300_HANDSHAKE_RETRY_MS + 1);
    io.clear_written();
    trace.take();
    // An idle session's keepalive.
    io.feed({0x05});
    pump(e);
    io.feed({0x06});
    pump(e);
    trace.take();
    advance(e, optolink::P300_KEEPALIVE_MS + 1);
    expect(trace, "*keepalive INIT INIT_ACK", "P300: the keepalive");
  }

  // --- GWG ----------------------------------------------------------------
  {
    using Engine = optolink::GwgExchange<FakeOptolink>;
    FakeOptolink io;
    Engine e(&io);
    Trace trace{&Engine::state_name, {}};
    e.on_result([](const optolink::Result &) {});
    e.set_sink(trace.sink());
    e.begin();
    expect(trace, "WAIT_ENQ", "GWG: begin");
    check(gwg_calls::read(e, 0x16, 1, optolink::GWGAccessMode::VIRTUAL), "GWG: read queued");
    io.feed({0x05});
    pump(e);
    expect(trace, "SEND RECEIVE", "GWG: the ENQ, 01 + telegram");
    io.feed({0x2A});
    pump(e);
    expect(trace, "WAIT_ENQ", "GWG: answered");
    // Burst-sent inside the window, met by the deadline: retried on the ENQ.
    check(gwg_calls::read(e, 0x17, 1, optolink::GWGAccessMode::VIRTUAL), "GWG: burst read queued");
    pump(e, 1);
    expect(trace, "SEND RECEIVE", "GWG: burst-sent");
    advance(e, optolink::GWG_ANSWER_TIMEOUT_MS + 1);
    expect(trace, "WAIT_ENQ *retry", "GWG: the deadline: retry on the ENQ");
    io.feed({0x05});
    pump(e);
    io.feed({0x2B});
    pump(e);
    trace.take();
    // A read met by the deadline, two bytes after it.
    optolink::optolink_test_clock_advance(optolink::GWG_BURST_WINDOW_MS + 1);
    check(gwg_calls::read(e, 0x18, 1, optolink::GWGAccessMode::VIRTUAL), "GWG: read queued");
    io.feed({0x05});
    pump(e);
    advance(e, optolink::GWG_ANSWER_TIMEOUT_MS + 1);
    io.feed({0x11, 0x22});
    pump(e);
    trace.take();
    advance(e, optolink::GWG_LATE_WATCH_MS + 1);
    expect(trace, "*late=2", "GWG: the late bytes reported");
    // A second failed burst after the first: bursting off.
    check(gwg_calls::read(e, 0x19, 1, optolink::GWGAccessMode::VIRTUAL), "GWG: read queued");
    io.feed({0x05});
    pump(e);
    io.feed({0x2C});
    pump(e);
    check(gwg_calls::read(e, 0x1A, 1, optolink::GWGAccessMode::VIRTUAL), "GWG: burst read queued");
    pump(e, 1);
    advance(e, optolink::GWG_ANSWER_TIMEOUT_MS + 1);
    io.feed({0x05});
    pump(e);
    io.feed({0x2D});
    pump(e);
    const std::string off = trace.take();
    check(off.find("*burst off=2") != std::string::npos, "GWG: the second failed burst: bursting off");
  }

  std::printf("link trace: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
