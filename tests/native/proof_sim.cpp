// The engines against the device simulators (sim/): each engine does its
// everyday work against a simulated controller, and each quirk switch has the
// effect the field finding behind it describes. The simulators are what the
// later proofs and the differential target build on, so this pins both: the
// engines as they are, and the simulators as models of the units we know.
//
//   K  KW: the KW exchange and SimVitotronic in KW
//   P  P300: the P300 exchange and SimVitotronic, the KW detour included
//   G  GWG: GWGEngine and SimGwg
//
// The clock is frozen; every scenario advances it 1 ms per engine pass.

#include <cstdio>
#include <cstring>
#include <vector>

#include "kw_calls.h"
#include "optolink/optolink.h"
#include "optolink_kw.h"
#include "sim/sim_gwg.h"
#include "sim/sim_vitotronic.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::OptolinkResult;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// What the callbacks saw.
struct Outcome {
  int responses = 0;
  int errors = 0;
  std::vector<uint8_t> data;
  uint16_t address = 0;
  OptolinkResult error = OptolinkResult::CONTINUE;
  // P300: the error frame's code of the last DEVICE_ERROR.
  std::vector<uint8_t> error_code;
  int done() const { return responses + errors; }
};

template<class E> void wire(E &e, Outcome &o) {
  kw_calls::wire(
      e,
      [&o](const uint8_t *data, uint8_t length, uint16_t address) {
        ++o.responses;
        o.data.assign(data, data != nullptr ? data + length : data);
        o.address = address;
      },
      [&o](OptolinkResult result, uint16_t address) {
        ++o.errors;
        o.error = result;
        o.address = address;
      });
  e.begin();
}

template<class E> void wire_p300(E &e, Outcome &o) {
  kw_calls::wire(
      e,
      [&o](const uint8_t *data, uint8_t length, uint16_t address) {
        ++o.responses;
        o.data.assign(data, data != nullptr ? data + length : data);
        o.address = address;
      },
      [&o](OptolinkResult result, uint16_t address) {
        ++o.errors;
        o.error = result;
        o.address = address;
      },
      [&o](const uint8_t *code, uint8_t n) { o.error_code.assign(code, code + n); });
  e.begin();
}

// Loops the engine, 1 ms per pass, until one more operation completed or
// `max_ms` passed; returns the ms it took.
template<class E> uint32_t finish(E &e, const Outcome &o, uint32_t max_ms, uint32_t step_ms = 1) {
  const int before = o.done();
  for (uint32_t t = 0; t < max_ms; t += step_ms) {
    e.loop();
    if (o.done() > before)
      return t;
    optolink::optolink_test_clock_advance(step_ms);
  }
  return max_ms;
}

template<class E> void idle(E &e, uint32_t ms) {
  for (uint32_t t = 0; t < ms; ++t) {
    e.loop();
    optolink::optolink_test_clock_advance(1);
  }
}

bool bytes_are(const std::vector<uint8_t> &got, std::initializer_list<uint8_t> want) {
  return got == std::vector<uint8_t>(want);
}

// Whether `pattern` occurs in `bytes`.
bool contains(const std::vector<uint8_t> &bytes, std::initializer_list<uint8_t> pattern) {
  const std::vector<uint8_t> p(pattern);
  for (std::size_t i = 0; i + p.size() <= bytes.size(); ++i) {
    if (std::equal(p.begin(), p.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i)))
      return true;
  }
  return false;
}

sim::VitotronicConfig kw_only() {
  sim::VitotronicConfig c;
  c.p300 = false;
  return c;
}

void kw() {
  std::printf("-- K: KW --\n");
  {
    sim::SimVitotronic dev(kw_only());
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 5000);
    check(o.responses == 1 && bytes_are(o.data, {0x20, 0xCB, 0x03, 0x51}), "K1: the ident on the first ENQ");
    check(contains(dev.written(), {0x01, 0xF7, 0x00, 0xF8, 0x04}), "K1: sent as 01 F7 00 F8 04");

    const uint32_t enqs = dev.enq_count();
    for (uint16_t a : {0x5525, 0x0810, 0x55D3}) {
      kw_calls::read(e, a, 2);
      finish(e, o, 5000);
    }
    check(o.responses == 4 && dev.enq_count() == enqs, "K2: three reads chained on that one ENQ");
    check(contains(dev.written(), {0xF7, 0x08, 0x10, 0x02}) && !contains(dev.written(), {0x01, 0xF7, 0x08, 0x10}),
          "K2: a chained telegram carries no 01");

    const uint8_t value[2] = {0x12, 0x34};
    kw_calls::write(e, 0x2306, value, 2);
    finish(e, o, 5000);
    kw_calls::read(e, 0x2306, 2);
    finish(e, o, 5000);
    check(o.responses == 6 && bytes_are(o.data, {0x12, 0x34}), "K3: a write, read back");

    dev.gfa.set(0x4050, 0x24);
    kw_calls::gfa_read(e, 0x4050, 1);
    finish(e, o, 5000);
    check(o.responses == 7 && bytes_are(o.data, {0x24}), "K4: a GFA read answers from the burner control");
  }
  {
    // The device takes a chained telegram for chain_listen_ms after its
    // answer. A read due 1 s after an answer, past the link's chain window,
    // waits for the ENQ instead of going out unheard.
    const sim::VitotronicConfig c = kw_only();
    check(optolink::KW_LINK_PARAMS.chain_window_ms < c.chain_listen_ms,
          "K9: the chain window ends before the device stops listening");
    sim::SimVitotronic dev(c);
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 5000);
    const uint32_t enqs = dev.enq_count();
    idle(e, 1000);
    kw_calls::read(e, 0x5525, 2);
    finish(e, o, 5000);
    check(o.responses == 2 && o.errors == 0 && dev.enq_count() == enqs + 1,
          "K9: a read 1 s after an answer goes out on the next ENQ and is answered");
  }
  {
    sim::VitotronicConfig c = kw_only();
    c.quirks.refuse_chain = true;
    sim::SimVitotronic dev(c);
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 5000);
    // The chained read meets silence and goes out again on the next ENQ.
    kw_calls::read(e, 0x5525, 2);
    finish(e, o, 10000);
    check(o.responses == 2 && o.errors == 0 && dev.enq_count() == 2,
          "K5: refuse_chain: the chained read is answered on the next ENQ");
  }
  {
    sim::VitotronicConfig c = kw_only();
    c.quirks.answer_delay_ms = optolink::KW_LINK_PARAMS.answer_timeout_ms + 200;
    sim::SimVitotronic dev(c);
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 8000);
    check(o.errors == 1 && o.error == OptolinkResult::TIMEOUT,
          "K6: an answer later than the answer deadline times out");
  }
  {
    // #35's finding B as a model: the 01 must follow the ENQ within 5 ms, and
    // a stalled loop reads the ENQ 7 ms late. The device ignores the 01, takes
    // the telegram's length byte 04 for an EOT and offers its next ENQ during
    // the answer wait; the retry goes out on the ENQ after it, read at once.
    // Such a unit needs ENQ_TO_SEND 0: the default 20 ms would miss its window.
    sim::VitotronicConfig c = kw_only();
    c.quirks.enq_window_ms = 5;
    sim::SimVitotronic dev(c);
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    uint32_t no_pause = 0;
    e.set_timing(optolink::TimingPoint::ENQ_TO_SEND, no_pause);
    Outcome o;
    wire(e, o);
    kw_calls::read(e, 0x00F8, 4);
    optolink::optolink_test_clock_advance(c.enq_first_ms + 7);
    finish(e, o, 8000);
    check(o.responses == 1 && o.errors == 0 && dev.answered() == 1,
          "K7: enq_window_ms: a 01 sent 7 ms late is ignored, the retry is answered");
  }
  {
    // A dead phase longer than the link's EOT interval: it sends an EOT and
    // gets the ident once the device is back.
    sim::VitotronicConfig c = kw_only();
    c.quirks.dead_period_ms = 60000;
    c.quirks.dead_ms = optolink::KW_LINK_PARAMS.eot_interval_ms + 2000;
    sim::SimVitotronic dev(c);
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 7000, 10);
    check(o.errors == 1, "K8: a dead phase: the read times out first");
    check(contains(dev.written(), {0x04}), "K8:   and the link sends an EOT while no ENQ comes");
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 5000);
    check(o.responses == 1 && bytes_are(o.data, {0x20, 0xCB, 0x03, 0x51}), "K8:   once the device is back it answers");
  }
}

// The device leaves a telegram unanswered (ignore_next_telegram): the link
// sends it once more on the next ENQ, and reports how often it did.
void kw_ignored() {
  std::printf("-- K: KW, ignored telegrams --\n");
  for (int ignored = 1; ignored <= 2; ++ignored) {
    sim::SimVitotronic dev(kw_only());
    optolink::KwExchange<sim::SimVitotronic> e(&dev);
    int responses = 0;
    int errors = 0;
    uint8_t retries = 0;
    e.on_result([&](const optolink::Result &r) {
      ++(r.status == OptolinkResult::OK ? responses : errors);
      retries = r.retries;
    });
    e.begin();
    kw_calls::read(e, 0x00F8, 4);
    idle(e, 2000);
    dev.ignore_next_telegram();
    kw_calls::read(e, 0x5525, 2);
    // The read goes out on the ENQ about 1.5 s from here and is ignored; its
    // retry waits for the next ENQ, about 3 s after it.
    idle(e, 3000);
    if (ignored == 2)
      dev.ignore_next_telegram();
    idle(e, 6000);
    if (ignored == 1) {
      check(responses == 2 && errors == 0 && retries == 1, "K10: an ignored read goes out again and is answered");
    } else {
      check(responses == 1 && errors == 1 && retries == 1, "K11: ignored twice: TIMEOUT after the one retry");
    }
  }
}

void p300() {
  std::printf("-- P: P300 --\n");
  {
    sim::VitotronicConfig c;
    c.unserved = {{0x7000, 0x70FF}};
    c.rpc_answer = {0x01, 0x02, 0x03};
    sim::SimVitotronic dev(c);
    optolink::P300Exchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire_p300(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 5000);
    check(o.responses == 1 && bytes_are(o.data, {0x20, 0xCB, 0x03, 0x51}) && dev.in_p300(),
          "P1: the ident, after the session handshake");
    check(contains(dev.written(), {0x04}) && contains(dev.written(), {0x16, 0x00, 0x00}) &&
              contains(dev.written(), {0x41, 0x05, 0x00, 0x01, 0x00, 0xF8, 0x04, 0x02}),
          "P1:   04, 16 00 00, then 41 05 00 01 00 F8 04 02");

    const uint8_t value = 0x25;
    kw_calls::write(e, 0x2306, &value, 1);
    finish(e, o, 5000);
    kw_calls::read(e, 0x2306, 1);
    finish(e, o, 5000);
    check(o.responses == 3 && bytes_are(o.data, {0x25}), "P2: a write, read back");

    const uint8_t parameter[2] = {0xAA, 0xBB};
    kw_calls::rpc(e, 0x0B00, parameter, 2);
    finish(e, o, 5000);
    check(o.responses == 4 && bytes_are(o.data, {0x01, 0x02, 0x03}), "P3: a procedure call returns rpc_answer");

    kw_calls::read(e, 0x7010, 2);
    finish(e, o, 5000);
    check(o.errors == 1 && o.error == OptolinkResult::DEVICE_ERROR && bytes_are(o.error_code, {0x01}),
          "P4: an unserved address: an error frame with code 01");

    dev.gfa.set(0x4050, 0x24);
    kw_calls::gfa_read(e, 0x4050, 1);
    finish(e, o, 8000);
    check(o.responses == 5 && bytes_are(o.data, {0x24}), "P5: a GFA read over the KW detour");
    idle(e, 1000);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 8000);
    check(o.responses == 6 && dev.in_p300(), "P5:   then the session is back for a P300 read");

    const std::size_t before = dev.written().size();
    idle(e, optolink::P300_KEEPALIVE_MS + 500);
    const std::vector<uint8_t> tail(dev.written().begin() + static_cast<std::ptrdiff_t>(before), dev.written().end());
    check(contains(tail, {0x16, 0x00, 0x00}) && dev.in_p300(), "P6: an idle session gets a keepalive and stays");
  }
  {
    sim::VitotronicConfig c;
    c.quirks.swallow_ack = true;
    sim::SimVitotronic dev(c);
    optolink::P300Exchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire_p300(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 10000);
    check(o.errors == 1 && o.error == OptolinkResult::TIMEOUT, "P7: swallow_ack: no 06, the read times out");
    dev.quirks().swallow_ack = false;
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 10000);
    check(o.responses == 1, "P7:   and the next one, with the 06 back, is answered");
  }
  {
    sim::VitotronicConfig c;
    c.quirks.answer_missing = 1;
    sim::SimVitotronic dev(c);
    optolink::P300Exchange<sim::SimVitotronic> e(&dev);
    Outcome o;
    wire_p300(e, o);
    kw_calls::read(e, 0x00F8, 4);
    finish(e, o, 10000);
    check(o.errors == 1 && o.error == OptolinkResult::TIMEOUT, "P8: answer_missing: a frame without its checksum");
  }
}

void gwg() {
  std::printf("-- G: GWG --\n");
  {
    sim::GwgConfig c;
    c.eeprom_unserved[0x17] = true;
    sim::SimGwg dev(c);
    optolink::GWGEngine e(&dev);
    Outcome o;
    wire(e, o);
    e.read(0x05, 2);
    finish(e, o, 5000);
    check(o.responses == 1 && bytes_are(o.data, {dev.memory[0][0x05], dev.memory[0][0x06]}),
          "G1: a physical read on the ENQ");

    const uint32_t enqs = dev.enq_count();
    e.read(0x06, 1);
    finish(e, o, 5000);
    check(o.responses == 2 && dev.enq_count() == enqs && !e.lastSendWasEnqPaced(),
          "G2: the next read rides the burst window");

    const uint8_t value = 0x42;
    e.write(0x30, &value, 1);
    finish(e, o, 5000);
    e.read(0x30, 1);
    finish(e, o, 5000);
    check(o.responses == 4 && bytes_are(o.data, {0x42}), "G3: a physical write, read back");

    dev.memory[2][0x15] = 0x9A;
    dev.memory[2][0x16] = 0x01;
    e.read(0x15, 3, optolink::GWGAccessMode::EEPROM);
    finish(e, o, 10000);
    check(o.responses == 5 && bytes_are(o.data, {0x9A, 0x01, 0xFF}) && e.lastReadHadUnservedEeprom(),
          "G4: EEPROM cells as pairs, an unserved one as FF");

    const uint8_t cell = 0x5C;
    e.write(0x20, &cell, 1, optolink::GWGAccessMode::EEPROM);
    finish(e, o, 5000);
    check(o.responses == 6 && dev.memory[2][0x20] == 0x5C, "G5: an EEPROM write as AD AD ~V V 04");
  }
  {
    sim::GwgConfig c;
    c.quirks.refuse_chain = true;
    sim::SimGwg dev(c);
    optolink::GWGEngine e(&dev);
    Outcome o;
    int fallbacks = 0;
    wire(e, o);
    e.onBurstFallback([&fallbacks](uint8_t) { ++fallbacks; });
    for (uint16_t a = 0; a < 5; ++a) {
      e.read(a, 1);
      finish(e, o, 10000);
    }
    check(o.responses == 5 && o.errors == 0, "G6: refuse_chain: every read still answered, on an ENQ");
    check(fallbacks == 1, "G6:   after two refused bursts the engine stops bursting");
  }
  {
    sim::GwgConfig c;
    c.quirks.answer_delay_ms = optolink::GWGEngine::RESPONSE_TIMEOUT_MS + 60;
    sim::SimGwg dev(c);
    optolink::GWGEngine e(&dev);
    Outcome o;
    int late = 0;
    wire(e, o);
    e.onLateAnswer([&late](const optolink::GWGEngine::LateAnswer &report) { late += report.late_count; });
    e.read(0x05, 2);
    finish(e, o, 10000);
    idle(e, optolink::GWGEngine::LATE_WATCH_MS + 200);  // the report comes when the watch closes
    check(o.errors == 1 && o.error == OptolinkResult::TIMEOUT, "G7: an answer past the deadline times out");
    check(late == 2, "G7:   and the late-answer report holds its two bytes");
  }
  {
    // #35's finding B as a model: the device takes the 01 only within 20 ms of
    // its ENQ, so a send delay of 30 ms goes unanswered and 10 ms works.
    for (uint32_t delay : {30u, 10u}) {
      sim::GwgConfig c;
      c.quirks.enq_window_ms = 20;
      sim::SimGwg dev(c);
      optolink::GWGEngine e(&dev);
      Outcome o;
      wire(e, o);
      e.setEnqSendDelayMs(delay);
      e.read(0x05, 1);
      finish(e, o, 5000);
      check(delay == 30 ? o.errors == 1 : o.responses == 1,
            delay == 30 ? "G8: enq_window_ms 20: sent 30 ms after the ENQ, no answer"
                        : "G8:   sent 10 ms after it, answered");
    }
  }
}

}  // namespace

int main() {
  optolink::optolink_test_clock_freeze();
  kw();
  kw_ignored();
  p300();
  gwg();
  std::printf("simulators: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
