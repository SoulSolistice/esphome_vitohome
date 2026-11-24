// The hub's requests (address_space.h: read_request(), write_request()) on its
// engine (protocol_select.h: SelectedEngine). Compiled once per protocol by
// build_and_run_protocols.sh.
//
// P300 and GWG: the hub's engine on a fake port and the frozen clock, given
// the hub's request and a scripted device. Each case must accept or refuse
// the request, write the bytes, report the outcomes and hold the loop as its
// recorded case does (fixtures/hub_requests_{p300,gwg}.txt, golden.h): the
// outcomes of the engine calls these requests replaced, with what the hub
// read inside their callbacks (the error code, an unsynced timeout, an
// unserved EEPROM read, the GWG timing). Each case also names the outcomes it
// must reach, so a recording on the wrong path fails too.
//
// KW: each request equals the one the KW harnesses send (kw_calls.h), which
// the differential target holds to the recorded traces.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "address_space.h"
#include "fake_optolink.h"
#include "golden.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using optolink::OptolinkResult;
using Bytes = std::vector<uint8_t>;
using Statuses = std::vector<OptolinkResult>;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

constexpr OptolinkResult kOk = OptolinkResult::OK;
constexpr OptolinkResult kTimeout = OptolinkResult::TIMEOUT;

// One reported outcome.
struct Event {
  OptolinkResult status{OptolinkResult::CONTINUE};
  uint16_t address{0};
  Bytes data;
  Bytes error_code;
  bool never_sent{false};
  bool unserved_eeprom{false};
  bool enq_paced{false};
  uint32_t enq_age_ms{0};
  uint32_t enq_to_send_ms{0};
  uint32_t ack_gap_ms{0};
  bool answer_timed{false};
  uint32_t send_to_answer_ms{0};
  bool operator==(const Event &o) const {
    return status == o.status && address == o.address && data == o.data && error_code == o.error_code &&
           never_sent == o.never_sent && unserved_eeprom == o.unserved_eeprom && enq_paced == o.enq_paced &&
           enq_age_ms == o.enq_age_ms && enq_to_send_ms == o.enq_to_send_ms && ack_gap_ms == o.ack_gap_ms &&
           answer_timed == o.answer_timed && send_to_answer_ms == o.send_to_answer_ms;
  }
};

Event from_result(const optolink::Result &r) {
  Event e;
  e.status = r.status;
  e.address = r.address;
  if (r.data != nullptr)
    e.data.assign(r.data, r.data + r.length);
  if (r.error_code != nullptr)
    e.error_code.assign(r.error_code, r.error_code + r.error_code_length);
  e.never_sent = r.never_sent;
  e.unserved_eeprom = r.unserved_eeprom;
  e.enq_paced = r.timing.enq_paced;
  e.enq_age_ms = r.timing.enq_age_ms;
  e.enq_to_send_ms = r.timing.enq_to_send_ms;
  e.ack_gap_ms = r.timing.ack_gap_ms;
  e.answer_timed = r.timing.answer_timed;
  e.send_to_answer_ms = r.timing.send_to_answer_ms;
  return e;
}

[[maybe_unused]] Golden *g_golden = nullptr;

[[maybe_unused]] std::string describe(const Event &e) {
  char line[192];
  std::snprintf(
      line, sizeof(line), "%s 0x%04X never_sent=%d unserved=%d paced=%d age=%u to_send=%u gap=%u timed=%d answer=%u",
      e.status == OptolinkResult::OK ? "ok" : optolink::errorToString(e.status), e.address, e.never_sent,
      e.unserved_eeprom, e.enq_paced, static_cast<unsigned>(e.enq_age_ms), static_cast<unsigned>(e.enq_to_send_ms),
      static_cast<unsigned>(e.ack_gap_ms), e.answer_timed, static_cast<unsigned>(e.send_to_answer_ms));
  return std::string(line) + " data=" + golden_hex(e.data.data(), e.data.size()) +
         " code=" + golden_hex(e.error_code.data(), e.error_code.size());
}

// The hub's engine and what it did: whether each request was accepted, the
// bytes written, every Result, and per pass whether it held the loop awake
// and was busy.
struct Hub {
  FakeOptolink port;
  SelectedEngine<FakeOptolink> engine{&port};
  std::vector<Event> events;
  Golden::Lines accepted;
  Golden::Lines passes;
  bool held{false};

  Hub() {
    engine.on_result([this](const optolink::Result &r) { events.push_back(from_result(r)); });
    engine.begin();
  }

  // The request built and submitted.
  bool accept(bool ok) {
    accepted.push_back(ok ? "accepted" : "refused");
    return ok;
  }
  void pass() {
    engine.loop();
    const bool holding = engine.wants_fast_loop();
    held = held || holding;
    this->note_(holding ? (engine.busy() ? "held busy" : "held") : (engine.busy() ? "busy" : "idle"));
    optolink::optolink_test_clock_advance(1);
  }
  // The port held by a timed frame: time passes, the engine is not looped,
  // then restarts.
  void park(uint32_t ms) {
    optolink::optolink_test_clock_advance(ms);
    engine.begin();
  }
  void step(const Bytes &feed, int passes) {
    port.feed(feed);
    for (int i = 0; i < passes; ++i)
      this->pass();
  }
  // Long enough for every deadline and watchdog, 10 ms per pass.
  void settle(uint32_t ms = 8000) {
    for (uint32_t t = 0; t < ms; t += 10) {
      engine.loop();
      optolink::optolink_test_clock_advance(10);
    }
  }
  // Reported exactly `want`, and did what the recorded case `what` did.
  bool ended(const char *what, const Statuses &want) {
    Statuses got;
    for (const Event &e : events)
      got.push_back(e.status);
    if (got != want) {
      std::printf("    reported:");
      for (OptolinkResult s : got)
        std::printf(" %s", optolink::errorToString(s));
      std::printf("\n");
    }
    Golden::Lines lines = accepted;
    lines.push_back("wrote " + golden_hex(port.written().data(), port.written().size()));
    for (const Event &e : events)
      lines.push_back(describe(e));
    lines.insert(lines.end(), passes.begin(), passes.end());
    if (run_ != 0)
      lines.push_back(run_state_ + " x" + std::to_string(run_));
    const std::string diff = g_golden->check(what, lines);
    if (!diff.empty())
      std::printf("    %s\n", diff.c_str());
    return diff.empty() && got == want;
  }

 private:
  std::string run_state_;
  uint32_t run_{0};
  void note_(const char *state) {
    if (run_ != 0 && run_state_ == state) {
      ++run_;
      return;
    }
    if (run_ != 0)
      passes.push_back(run_state_ + " x" + std::to_string(run_));
    run_state_ = state;
    run_ = 1;
  }
};

[[maybe_unused]] bool same(const optolink::Request &a, const optolink::Request &b) {
  const std::size_t n = a.length < sizeof(a.payload) ? a.length : sizeof(a.payload);
  return a.lane == b.lane && a.code == b.code && a.address == b.address && a.length == b.length &&
         a.has_payload == b.has_payload && a.answer_length == b.answer_length &&
         (!a.has_payload || std::memcmp(a.payload, b.payload, n) == 0);
}

// What every protocol shares: a space without a telegram, a write without
// data, a procedure call without its parameter.
void common_refusals() {
  optolink::Request r{};
  const uint8_t data[] = {0x01, 0x02};
  check(!write_request(AddressSpace::OWN, 0x0800, nullptr, 1, nullptr, 0, r), "a write without data is refused");
  check(!write_request(AddressSpace::GFA, 0x4050, data, 1, nullptr, 0, r), "a GFA write is refused");
  check(!read_request(AddressSpace::RPC, 0x0800, 0, nullptr, 2, r),
        "a procedure call without its parameter is refused");
  check(read_request(AddressSpace::RPC, 0x0800, 0, data, 2, r) == IS_P300, "a procedure call only on P300");
  check(read_request(AddressSpace::GFA, 0x4050, 1, nullptr, 0, r) == (IS_KW || IS_P300), "a GFA read on KW and P300");
  check(read_request(AddressSpace::GWG_EEPROM, 0x10, 1, nullptr, 0, r) == IS_GWG, "a GWG mode only on GWG");
  check(!write_request(AddressSpace::GWG_KMBUS_RAM, 0x10, data, 1, nullptr, 0, r), "a KM-bus write is refused");
}

#if defined(VITOHOME_PROTOCOL_KW)

void kw_cases() {
  std::printf("-- KW --\n");
  FakeOptolink port;
  SelectedEngine<FakeOptolink> kw(&port);
  const uint8_t data[] = {0x12, 0x34};
  optolink::Request r{};
  check(read_request(AddressSpace::OWN, 0x00F8, 4, nullptr, 0, r) &&
            same(r, optolink::make_request(optolink::Lane::KW, optolink::PacketVS1Type.READ, 0x00F8, 4, nullptr)),
        "OWN read: kw_calls::read()'s request (F7)");
  check(write_request(AddressSpace::OWN, 0x2306, data, 2, nullptr, 0, r) &&
            same(r, optolink::make_request(optolink::Lane::KW, optolink::PacketVS1Type.WRITE, 0x2306, 2, data)),
        "OWN write: kw_calls::write()'s request (F4)");
  check(read_request(AddressSpace::GFA, 0x4050, 1, nullptr, 0, r) &&
            same(r, optolink::make_request(optolink::Lane::KW, optolink::PacketVS1Type.GFA_READ, 0x4050, 1, nullptr)),
        "GFA read: kw_calls::gfa_read()'s request (6B)");
  check(!kw.submit(optolink::make_request(optolink::Lane::P300, 0x01, 0x00F8, 1, nullptr)) &&
            !kw.submit(optolink::make_request(optolink::Lane::GWG, 0xCB, 0x00F8, 1, nullptr)),
        "the KW exchange refuses the P300 and GWG lanes");
  check(!kw.wants_fast_loop(), "and never holds the loop awake");

  std::vector<Event> events;
  kw.on_result([&events](const optolink::Result &res) { events.push_back(from_result(res)); });
  kw.begin();
  check(read_request(AddressSpace::OWN, 0x00F8, 1, nullptr, 0, r) && kw.submit(r), "a read accepted");
  // 1 ms per pass, past the link's pauses.
  port.feed({0x05});
  for (int i = 0; i < 40; ++i) {
    kw.loop();
    optolink::optolink_test_clock_advance(1);
  }
  port.feed({0x20});
  for (int i = 0; i < 40; ++i) {
    kw.loop();
    optolink::optolink_test_clock_advance(1);
  }
  Event want;
  want.status = OptolinkResult::OK;
  want.address = 0x00F8;
  want.data = {0x20};
  check(events.size() == 1 && events[0] == want, "its result: OK, the data, nothing else set");
}

#elif defined(VITOHOME_PROTOCOL_GWG)

// The bytes an EEPROM cell answers: the complement, then the value.
Bytes cells(std::initializer_list<uint8_t> values) {
  Bytes out;
  for (uint8_t v : values) {
    out.push_back(static_cast<uint8_t>(~v));
    out.push_back(v);
  }
  return out;
}

struct GwgKnobs {
  uint32_t delay_ms{0};
  uint32_t gap_ms{0};
  bool eot_first{false};
};

void apply(Hub &t, const GwgKnobs &k) {
  // set_timing() clamps its argument in place.
  uint32_t delay_ms = k.delay_ms;
  uint32_t gap_ms = k.gap_ms;
  t.engine.set_timing(optolink::TimingPoint::ENQ_TO_SEND, delay_ms);
  t.engine.set_timing(optolink::TimingPoint::ACK_TO_TELEGRAM, gap_ms);
  t.engine.set_write_eot_before_payload(k.eot_first);
}

// A read; then `script`, 40 passes per chunk.
bool gwg_read(Hub &t, AddressSpace space, uint16_t address, uint8_t length, const std::vector<Bytes> &script) {
  optolink::Request r{};
  const bool ok = t.accept(read_request(space, address, length, nullptr, 0, r) && t.engine.submit(r));
  for (const Bytes &b : script)
    t.step(b, 40);
  return ok;
}

bool gwg_write(Hub &t, AddressSpace space, uint16_t address, const Bytes &data, const std::vector<Bytes> &script) {
  optolink::Request r{};
  const auto n = static_cast<uint8_t>(data.size());
  const bool ok = t.accept(write_request(space, address, data.data(), n, nullptr, 0, r) && t.engine.submit(r));
  for (const Bytes &b : script)
    t.step(b, 40);
  return ok;
}

void read_case(const char *what, const Statuses &want, AddressSpace space, uint16_t address, uint8_t length,
               const std::vector<Bytes> &script, const GwgKnobs &knobs = {}) {
  Hub t;
  apply(t, knobs);
  gwg_read(t, space, address, length, script);
  t.settle();
  check(t.ended(what, want), what);
}

void write_case(const char *what, const Statuses &want, AddressSpace space, uint16_t address, const Bytes &data,
                const std::vector<Bytes> &script, const GwgKnobs &knobs = {}) {
  Hub t;
  apply(t, knobs);
  gwg_write(t, space, address, data, script);
  t.settle();
  check(t.ended(what, want), what);
}

void gwg_cases() {
  std::printf("-- GWG --\n");
  read_case("OWN read", {kOk}, AddressSpace::OWN, 0x55, 2, {{0x05}, {0x12, 0x34}});
  read_case("VIRTUAL read (identification)", {kOk}, AddressSpace::GWG_VIRTUAL, 0xF8, 4,
            {{0x05}, {0x20, 0x53, 0x01, 0x1E}});
  read_case("EEPROM read, one address per telegram", {kOk}, AddressSpace::GWG_EEPROM, 0x15, 3,
            {{0x05}, cells({0x0F}), cells({0x11}), cells({0x22})});
  read_case("EEPROM pair that does not sum to FF: CRC", {OptolinkResult::CRC}, AddressSpace::GWG_EEPROM, 0x15, 1,
            {{0x05}, {0x12, 0x34}});
  read_case("BE read", {kOk}, AddressSpace::GWG_BE, 0x20, 2, {{0x05}, {0x01}, {0x02}});
  read_case("KM-bus RAM read", {kOk}, AddressSpace::GWG_KMBUS_RAM, 0x30, 1, {{0x05}, {0x7F}});
  read_case("address past FF: refused", {}, AddressSpace::OWN, 0x100, 1, {});
  {
    Hub t;
    gwg_read(t, AddressSpace::GWG_EEPROM, 0x15, 2, {{0x05}, cells({0x0F}), {0xFF, 0xFF}});
    t.settle();
    const char *what = "EEPROM FF FF: OK, flagged unserved";
    check(t.ended(what, {kOk}) && t.events[0].unserved_eeprom, what);
  }
  {
    Hub t;
    gwg_read(t, AddressSpace::OWN, 0x55, 1, {});
    t.settle();
    const char *what = "TIMEOUT before any ENQ: never sent";
    check(t.ended(what, {kTimeout}) && t.events[0].never_sent, what);
  }
  {
    Hub t;
    gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}});
    t.settle();
    const char *what = "TIMEOUT after an ENQ: sent, ENQ-paced";
    check(t.ended(what, {kTimeout}) && !t.events[0].never_sent && t.events[0].enq_paced, what);
  }
  {
    Hub t;
    apply(t, GwgKnobs{20, 0, false});
    gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}, {}, {0x42}});
    t.settle();
    const char *what = "an ENQ-to-send delay: held, the hold agrees";
    check(t.ended(what, {kOk}) && t.held, what);
  }
  {
    Hub t;
    apply(t, GwgKnobs{0, 30, false});
    gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}, {}, {0x42}});
    t.settle();
    const char *what = "a gap after the 01: held, the gap measured";
    check(t.ended(what, {kOk}) && t.held && t.events[0].ack_gap_ms != 0, what);
  }
  {
    // A read right after a restart: its watchdog runs from the restart.
    Hub t;
    t.step({}, 8);
    t.park(2500);
    gwg_read(t, AddressSpace::OWN, 0x55, 1, {});
    for (int i = 0; i < 25; ++i)
      t.step({}, 40);
    t.step({0x05}, 40);
    t.step({0x42}, 40);
    t.settle();
    const char *what = "after a 2.5 s park: no early TIMEOUT, answered on the ENQ";
    check(t.ended(what, {kOk}), what);
  }
  {
    // A read's timing is its own: the write after it reports none.
    Hub t;
    gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}, {0x42}});
    gwg_write(t, AddressSpace::OWN, 0x30, {0x07}, {{0x05}, {0x00}});
    t.settle();
    const char *what = "a read, then a write on the same engine";
    check(t.ended(what, {kOk, kOk}), what);
    check(t.events.size() == 2 && t.events[0].answer_timed && !t.events[1].answer_timed, "  only the read is timed");
  }

  write_case("OWN write", {kOk}, AddressSpace::OWN, 0x30, {0x05}, {{0x05}, {0x00}});
  write_case("OWN write, EOT before the payload", {kOk}, AddressSpace::OWN, 0x30, {0x05, 0x06}, {{0x05}, {0x00}},
             GwgKnobs{0, 0, true});
  write_case("EEPROM write of one cell", {kOk}, AddressSpace::GWG_EEPROM, 0x20, {0x0F}, {{0x05}, {0x00}});
  write_case("EEPROM write of two bytes: refused", {}, AddressSpace::GWG_EEPROM, 0x20, {0x0F, 0xF0}, {});
  write_case("BE write", {kOk}, AddressSpace::GWG_BE, 0x20, {0x01}, {{0x05}, {0x00}});
  write_case("KM-bus write: refused", {}, AddressSpace::GWG_KMBUS_EEPROM, 0x20, {0x01}, {});

  {
    Hub t;
    const uint8_t pair[] = {0xF0, 0x0F};
    const bool ok = t.accept(t.engine.submit(optolink::make_request(optolink::Lane::GWG, 0xAD, 0x20, 2, pair)));
    t.step({0x05}, 40);
    t.step({0x00}, 40);
    t.settle();
    const char *what = "raw lane: a two-byte EEPROM write is the pair as given";
    check(ok && t.ended(what, {kOk}), what);
  }
  {
    Hub t;
    const bool ok = t.accept(t.engine.submit(optolink::make_request(optolink::Lane::GWG, 0x31, 0x05, 1, nullptr)));
    t.step({0x05}, 40);
    t.step({0x7F}, 40);
    t.settle();
    const char *what = "raw lane: an unnamed TYPE byte";
    check(ok && t.ended(what, {kOk}), what);
  }
  {
    Hub t;
    const uint8_t bytes[] = {0x01, 0xCB, 0x55, 0x01, 0x04};
    const bool ok = t.accept(t.engine.submit(optolink::make_frame(bytes, sizeof(bytes), 0)));
    t.step({0x05}, 40);
    t.step({0x42, 0x43}, 40);
    t.settle();
    const char *what = "open frame, collected until quiet";
    check(ok && t.ended(what, {kOk}), what);
  }
  {
    Hub t;
    check(!t.engine.submit(optolink::make_request(optolink::Lane::P300, 0x01, 0x55, 1, nullptr)) &&
              !t.engine.submit(optolink::make_request(optolink::Lane::KW, 0xF7, 0x55, 1, nullptr)),
          "the GWG exchange refuses the P300 and KW lanes");
    check(!t.engine.submit(optolink::make_frame(nullptr, 3, 1)), "and an open frame without bytes");
  }
}

#else

// A P300 frame with its checksum, the sum of every byte after the 41.
Bytes p300(const Bytes &body) {
  uint8_t cs = 0;
  for (uint8_t b : body)
    cs = static_cast<uint8_t>(cs + b);
  Bytes frame{0x41};
  frame.insert(frame.end(), body.begin(), body.end());
  frame.push_back(cs);
  return frame;
}

Bytes acked(const Bytes &frame) {
  Bytes out{0x06};
  out.insert(out.end(), frame.begin(), frame.end());
  return out;
}

// The link up (the EOT, the device's 05, our 16 00 00, its 06), then the
// request, then `script` once the request is out.
bool p300_run(Hub &t, bool built, const optolink::Request &r, const std::vector<Bytes> &script) {
  t.step({}, 8);
  t.step({0x05}, 8);
  t.step({0x06}, 8);
  const bool ok = t.accept(built && t.engine.submit(r));
  // The request goes out before anything answers it.
  t.step({}, 8);
  for (const Bytes &b : script)
    t.step(b, 8);
  t.settle();
  return ok;
}

void p300_case(const char *what, const Statuses &want, bool built, const optolink::Request &r,
               const std::vector<Bytes> &script) {
  Hub t;
  p300_run(t, built, r, script);
  check(t.ended(what, want), what);
}

void p300_cases() {
  std::printf("-- P300 --\n");
  optolink::Request r{};
  const uint8_t data[] = {0x03, 0x04};
  const uint8_t param[] = {0x01, 0x02};

  {
    Hub t;
    const bool b = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
    p300_run(t, b, r, {acked(p300({0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB}))});
    const char *what = "OWN read";
    check(t.ended(what, {kOk}) && t.events[0].data == Bytes({0x20, 0xCB}), what);
  }
  {
    Hub t;
    const bool b = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
    p300_run(t, b, r, {acked(p300({0x06, 0x03, 0x01, 0x00, 0xF8, 0x02, 0x05}))});
    const char *what = "an error frame: DEVICE_ERROR with its code 05";
    check(t.ended(what, {OptolinkResult::DEVICE_ERROR}) && t.events[0].error_code == Bytes({0x05}), what);
  }
  bool b = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
  p300_case("no answer: TIMEOUT", {kTimeout}, b, r, {});
  b = read_request(AddressSpace::OWN, 0x2000, optolink::MAX_PAYLOAD + 1, nullptr, 0, r);
  p300_case("a read past MAX_PAYLOAD: refused", {}, b, r, {});

  b = write_request(AddressSpace::OWN, 0x2306, data, 2, nullptr, 0, r);
  p300_case("OWN write", {kOk}, b, r, {acked(p300({0x05, 0x01, 0x02, 0x23, 0x06, 0x02}))});

  b = read_request(AddressSpace::RPC, 0x1234, 4, param, 2, r);
  p300_case("procedure call with its parameter", {kOk}, b, r,
            {acked(p300({0x07, 0x01, 0x07, 0x12, 0x34, 0x02, 0xAA, 0xBB}))});
  b = write_request(AddressSpace::RPC, 0x1234, data, 2, nullptr, 0, r);
  p300_case("an action button: the call without parameter", {kOk}, b, r,
            {acked(p300({0x05, 0x01, 0x07, 0x12, 0x34, 0x00}))});

  {
    Hub t;
    const bool built = read_request(AddressSpace::GFA, 0x4050, 1, nullptr, 0, r);
    p300_run(t, built, r, {{}, {0x05}, {0x24}, {}, {0x05}, {0x06}});
    const char *what = "GFA read: the KW detour";
    check(t.ended(what, {kOk}) && t.events[0].data == Bytes({0x24}), what);
  }

  r = optolink::make_request(optolink::Lane::P300, 0x05, 0x1000, 2, nullptr);
  p300_case("raw lane: a generic function code", {kOk}, true, r,
            {acked(p300({0x07, 0x01, 0x05, 0x10, 0x00, 0x02, 0xAA, 0xBB}))});
  r = optolink::make_request(optolink::Lane::KW, 0xF4, 0x2306, 1, data);
  p300_case("raw lane: a KW telegram with payload", {kOk}, true, r, {{}, {0x05}, {0x00}, {}, {0x05}, {0x06}});
  const uint8_t frame[] = {0x16, 0x00, 0x00};
  r = optolink::make_frame(frame, sizeof(frame), 1);
  p300_case("open frame", {kOk}, true, r, {{0x06}});

  {
    // A read right after a restart: its watchdog runs from the restart.
    Hub t;
    t.step({}, 8);
    t.park(3500);
    const bool b2 = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
    const bool ok = t.accept(b2 && t.engine.submit(r));
    for (int i = 0; i < 8; ++i)
      t.step({}, 100);  // 800 ms before the device's ENQ
    t.step({0x05}, 8);
    t.step({0x06}, 8);
    t.step({}, 8);
    t.step(acked(p300({0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB})), 8);
    t.settle();
    const char *what = "after a 3.5 s park: no early TIMEOUT, answered";
    check(ok && t.ended(what, {kOk}), what);
  }

  Hub t;
  check(!t.engine.submit(optolink::make_request(optolink::Lane::GWG, 0xCB, 0x55, 1, nullptr)),
        "the P300 exchange refuses the GWG lane");
  check(!t.engine.wants_fast_loop(), "and never holds the loop awake");
}

#endif

}  // namespace

int main() {
  std::printf("== hub requests (%s) ==\n", PROTOCOL_NAME);
  optolink::optolink_test_clock_freeze();
  common_refusals();
#if defined(VITOHOME_PROTOCOL_KW)
  kw_cases();
#else
#if defined(VITOHOME_PROTOCOL_GWG)
  Golden golden("hub_requests_gwg");
  g_golden = &golden;
  gwg_cases();
#else
  Golden golden("hub_requests_p300");
  g_golden = &golden;
  p300_cases();
#endif
  g_fail += golden.finish();
#endif
  std::printf("hub requests: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
