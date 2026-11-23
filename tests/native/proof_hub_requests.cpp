// The hub's requests (address_space.h: read_request(), write_request()) on its
// engine (protocol_select.h: SelectedEngine), against the engine calls they
// replace. Compiled once per protocol by build_and_run_protocols.sh.
//
// P300 and GWG: two engines on two fake ports, fed the same bytes on the same
// frozen clock. One is the old engine, called as the hub called it (P300: the
// reference copy's VS2Engine, tests/native/reference: read(), write(), rpc(),
// gfaRead(), request(), kwTelegram(), frame(); GWG: GWGEngine::read() and
// write() with the space's mode, telegram(), frame()); the other is the
// hub's engine, given the hub's request. Both must accept or refuse alike,
// write the same bytes and report the same outcomes, including what the hub
// read from the old engine's getters inside its callbacks: errorPayload(),
// lastTimeoutWasUnsynced(), lastReadHadUnservedEeprom(), the GWG timing,
// isHoldingEnq(). Each case also names the outcomes it must reach, so
// agreeing on the wrong path fails too.
//
// KW: each request equals the one the KW harnesses send (kw_calls.h), which
// the differential target holds to VS1Engine's read(), write() and gfaRead().
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include "address_space.h"
#include "fake_optolink.h"
#include "protocol_select.h"
#if !defined(VITOHOME_PROTOCOL_KW) && !defined(VITOHOME_PROTOCOL_GWG)
#include "reference/optolink/optolink.h"
using RefVs2 = esphome::vitohome::optolink_ref::VS2Engine;
#endif

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

// One reported outcome, from a Result or from an old engine's callback and
// getters.
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

// The old engine's callbacks, read as the hub read them. Each build uses its
// protocol's.
#if !defined(VITOHOME_PROTOCOL_KW) && !defined(VITOHOME_PROTOCOL_GWG)
// The reference's error codes have the live values; it has no OK.
void capture(RefVs2 &e, std::vector<Event> &out) {
  e.onResponse([&out](const uint8_t *data, uint8_t length, uint16_t address) {
    Event ev;
    ev.status = OptolinkResult::OK;
    ev.address = address;
    if (data != nullptr)
      ev.data.assign(data, data + length);
    out.push_back(ev);
  });
  e.onError([&e, &out](esphome::vitohome::optolink_ref::OptolinkResult status, uint16_t address) {
    Event ev;
    ev.status = static_cast<OptolinkResult>(static_cast<int>(status));
    ev.address = address;
    if (status == esphome::vitohome::optolink_ref::OptolinkResult::DEVICE_ERROR) {
      uint8_t n = 0;
      const uint8_t *code = e.errorPayload(n);
      ev.error_code.assign(code, code + n);
    }
    out.push_back(ev);
  });
}
#endif

struct GwgTiming {
  bool timed{false};
  uint32_t enq_age_ms{0};
  uint32_t send_to_response_ms{0};
};

[[maybe_unused]] void gwg_getters(const optolink::GWGEngine &e, Event &ev) {
  ev.enq_paced = e.lastSendWasEnqPaced();
  ev.enq_age_ms = e.lastEnqAgeMs();
  ev.enq_to_send_ms = e.lastEnqToSendMs();
  ev.ack_gap_ms = e.lastAckGapMs();
}

[[maybe_unused]] void capture(optolink::GWGEngine &e, std::vector<Event> &out, GwgTiming &t) {
  e.onTiming([&t](uint32_t enq_age_ms, uint32_t send_to_response_ms, uint16_t) {
    t.timed = true;
    t.enq_age_ms = enq_age_ms;
    t.send_to_response_ms = send_to_response_ms;
  });
  e.onResponse([&e, &out, &t](const uint8_t *data, uint8_t length, uint16_t address) {
    Event ev;
    ev.status = OptolinkResult::OK;
    ev.address = address;
    if (data != nullptr)
      ev.data.assign(data, data + length);
    ev.unserved_eeprom = e.lastReadHadUnservedEeprom();
    gwg_getters(e, ev);
    // The hub's timing line took the age from the timing callback.
    if (t.timed)
      ev.enq_age_ms = t.enq_age_ms;
    ev.answer_timed = t.timed;
    ev.send_to_answer_ms = t.send_to_response_ms;
    t = GwgTiming{};
    out.push_back(ev);
  });
  e.onError([&e, &out, &t](OptolinkResult status, uint16_t address) {
    Event ev;
    ev.status = status;
    ev.address = address;
    ev.never_sent = status == OptolinkResult::TIMEOUT && e.lastTimeoutWasUnsynced();
    gwg_getters(e, ev);
    t = GwgTiming{};
    out.push_back(ev);
  });
}

// The old engine and the hub's engine side by side.
template<class Old> struct Twin {
  FakeOptolink old_port;
  FakeOptolink new_port;
  Old old_engine{&old_port};
  SelectedEngine<FakeOptolink> engine{&new_port};
  std::vector<Event> old_events;
  std::vector<Event> new_events;
  GwgTiming gwg_timing;
  bool holds_agree{true};
  bool held{false};

  Twin() {
    if constexpr (std::is_same<Old, optolink::GWGEngine>::value) {
      capture(old_engine, old_events, gwg_timing);
    } else {
      capture(old_engine, old_events);
    }
    engine.on_result([this](const optolink::Result &r) { new_events.push_back(from_result(r)); });
    old_engine.begin();
    engine.begin();
  }

  void pass() {
    old_engine.loop();
    engine.loop();
    bool holding = false;
    if constexpr (std::is_same<Old, optolink::GWGEngine>::value)
      holding = old_engine.isHoldingEnq();
    held = held || holding;
    holds_agree = holds_agree && holding == engine.wants_fast_loop() && old_engine.isBusy() == engine.busy();
    optolink::optolink_test_clock_advance(1);
  }
  // The port held by a timed frame: time passes, neither engine is looped,
  // then both restart.
  void park(uint32_t ms) {
    optolink::optolink_test_clock_advance(ms);
    old_engine.begin();
    engine.begin();
  }
  void step(const Bytes &feed, int passes) {
    old_port.feed(feed);
    new_port.feed(feed);
    for (int i = 0; i < passes; ++i)
      this->pass();
  }
  // Long enough for every deadline and watchdog, 10 ms per pass.
  void settle(uint32_t ms = 8000) {
    for (uint32_t t = 0; t < ms; t += 10) {
      old_engine.loop();
      engine.loop();
      optolink::optolink_test_clock_advance(10);
    }
  }
  // Both wrote and reported the same, and reported exactly `want`.
  bool ended(const Statuses &want) const {
    Statuses got;
    for (const Event &e : new_events)
      got.push_back(e.status);
    if (got != want) {
      std::printf("    reported:");
      for (OptolinkResult s : got)
        std::printf(" %s", optolink::errorToString(s));
      std::printf("\n");
    }
    return holds_agree && old_port.written() == new_port.written() && old_events == new_events && got == want;
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

using GwgTwin = Twin<optolink::GWGEngine>;

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

void apply(GwgTwin &t, const GwgKnobs &k) {
  t.old_engine.setEnqSendDelayMs(k.delay_ms);
  t.engine.set_enq_send_delay_ms(k.delay_ms);
  t.old_engine.setEnqAckGapMs(k.gap_ms);
  t.engine.set_enq_ack_gap_ms(k.gap_ms);
  t.old_engine.setWriteEotBeforePayload(k.eot_first);
  t.engine.set_write_eot_before_payload(k.eot_first);
}

// A read on both engines: accepted alike; then `script`, 40 passes per chunk.
bool gwg_read(GwgTwin &t, AddressSpace space, uint16_t address, uint8_t length, const std::vector<Bytes> &script) {
  optolink::Request r{};
  const bool built = read_request(space, address, length, nullptr, 0, r);
  const bool old_ok = t.old_engine.read(address, length, gwg_mode(space));
  const bool new_ok = built && t.engine.submit(r);
  for (const Bytes &b : script)
    t.step(b, 40);
  return old_ok == new_ok;
}

bool gwg_write(GwgTwin &t, AddressSpace space, uint16_t address, const Bytes &data, const std::vector<Bytes> &script) {
  optolink::Request r{};
  const auto n = static_cast<uint8_t>(data.size());
  const bool built = write_request(space, address, data.data(), n, nullptr, 0, r);
  const bool old_ok = t.old_engine.write(address, data.data(), n, gwg_mode(space));
  const bool new_ok = built && t.engine.submit(r);
  for (const Bytes &b : script)
    t.step(b, 40);
  return old_ok == new_ok;
}

void read_case(const char *what, const Statuses &want, AddressSpace space, uint16_t address, uint8_t length,
               const std::vector<Bytes> &script, const GwgKnobs &knobs = {}) {
  GwgTwin t;
  apply(t, knobs);
  const bool alike = gwg_read(t, space, address, length, script);
  t.settle();
  check(alike && t.ended(want), what);
}

void write_case(const char *what, const Statuses &want, AddressSpace space, uint16_t address, const Bytes &data,
                const std::vector<Bytes> &script, const GwgKnobs &knobs = {}) {
  GwgTwin t;
  apply(t, knobs);
  const bool alike = gwg_write(t, space, address, data, script);
  t.settle();
  check(alike && t.ended(want), what);
}

void gwg_cases() {
  std::printf("-- GWG --\n");
  read_case("OWN read: same bytes, same result", {kOk}, AddressSpace::OWN, 0x55, 2, {{0x05}, {0x12, 0x34}});
  read_case("VIRTUAL read (identification)", {kOk}, AddressSpace::GWG_VIRTUAL, 0xF8, 4,
            {{0x05}, {0x20, 0x53, 0x01, 0x1E}});
  read_case("EEPROM read, one address per telegram", {kOk}, AddressSpace::GWG_EEPROM, 0x15, 3,
            {{0x05}, cells({0x0F}), cells({0x11}), cells({0x22})});
  read_case("EEPROM pair that does not sum to FF: CRC", {OptolinkResult::CRC}, AddressSpace::GWG_EEPROM, 0x15, 1,
            {{0x05}, {0x12, 0x34}});
  read_case("BE read", {kOk}, AddressSpace::GWG_BE, 0x20, 2, {{0x05}, {0x01}, {0x02}});
  read_case("KM-bus RAM read", {kOk}, AddressSpace::GWG_KMBUS_RAM, 0x30, 1, {{0x05}, {0x7F}});
  read_case("address past FF: both refuse", {}, AddressSpace::OWN, 0x100, 1, {});
  {
    GwgTwin t;
    const bool alike = gwg_read(t, AddressSpace::GWG_EEPROM, 0x15, 2, {{0x05}, cells({0x0F}), {0xFF, 0xFF}});
    t.settle();
    check(alike && t.ended({kOk}) && t.new_events[0].unserved_eeprom, "EEPROM FF FF: OK, flagged unserved");
  }
  {
    GwgTwin t;
    const bool alike = gwg_read(t, AddressSpace::OWN, 0x55, 1, {});
    t.settle();
    check(alike && t.ended({kTimeout}) && t.new_events[0].never_sent, "TIMEOUT before any ENQ: never sent");
  }
  {
    GwgTwin t;
    const bool alike = gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}});
    t.settle();
    check(alike && t.ended({kTimeout}) && !t.new_events[0].never_sent && t.new_events[0].enq_paced,
          "TIMEOUT after an ENQ: sent, ENQ-paced");
  }
  {
    GwgTwin t;
    apply(t, GwgKnobs{20, 0, false});
    const bool alike = gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}, {}, {0x42}});
    t.settle();
    check(alike && t.ended({kOk}) && t.held, "an ENQ-to-send delay: held, the hold agrees");
  }
  {
    GwgTwin t;
    apply(t, GwgKnobs{0, 30, false});
    const bool alike = gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}, {}, {0x42}});
    t.settle();
    check(alike && t.ended({kOk}) && t.held && t.new_events[0].ack_gap_ms != 0,
          "a gap after the 01: held, the gap measured");
  }
  {
    // A read right after a restart: its watchdog runs from the restart.
    GwgTwin t;
    t.step({}, 8);
    t.park(2500);
    const bool alike = gwg_read(t, AddressSpace::OWN, 0x55, 1, {});
    for (int i = 0; i < 25; ++i)
      t.step({}, 40);
    t.step({0x05}, 40);
    t.step({0x42}, 40);
    t.settle();
    check(alike && t.ended({kOk}), "after a 2.5 s park: no early TIMEOUT, answered on the ENQ");
  }
  {
    // A read's timing is its own: the write after it reports none.
    GwgTwin t;
    const bool read_alike = gwg_read(t, AddressSpace::OWN, 0x55, 1, {{0x05}, {0x42}});
    const bool write_alike = gwg_write(t, AddressSpace::OWN, 0x30, {0x07}, {{0x05}, {0x00}});
    t.settle();
    check(read_alike && write_alike && t.ended({kOk, kOk}), "a read, then a write on the same engine");
    check(t.new_events.size() == 2 && t.new_events[0].answer_timed && !t.new_events[1].answer_timed,
          "  only the read is timed");
  }

  write_case("OWN write", {kOk}, AddressSpace::OWN, 0x30, {0x05}, {{0x05}, {0x00}});
  write_case("OWN write, EOT before the payload", {kOk}, AddressSpace::OWN, 0x30, {0x05, 0x06}, {{0x05}, {0x00}},
             GwgKnobs{0, 0, true});
  write_case("EEPROM write of one cell", {kOk}, AddressSpace::GWG_EEPROM, 0x20, {0x0F}, {{0x05}, {0x00}});
  write_case("EEPROM write of two bytes: both refuse", {}, AddressSpace::GWG_EEPROM, 0x20, {0x0F, 0xF0}, {});
  write_case("BE write", {kOk}, AddressSpace::GWG_BE, 0x20, {0x01}, {{0x05}, {0x00}});
  write_case("KM-bus write: both refuse", {}, AddressSpace::GWG_KMBUS_EEPROM, 0x20, {0x01}, {});

  {
    GwgTwin t;
    const uint8_t pair[] = {0xF0, 0x0F};
    const bool old_ok = t.old_engine.telegram(0xAD, 0x20, 2, pair);
    const bool new_ok = t.engine.submit(optolink::make_request(optolink::Lane::GWG, 0xAD, 0x20, 2, pair));
    t.step({0x05}, 40);
    t.step({0x00}, 40);
    t.settle();
    check(old_ok && new_ok && t.ended({kOk}), "raw lane: a two-byte EEPROM write is the pair as given");
  }
  {
    GwgTwin t;
    const bool old_ok = t.old_engine.telegram(0x31, 0x05, 1);
    const bool new_ok = t.engine.submit(optolink::make_request(optolink::Lane::GWG, 0x31, 0x05, 1, nullptr));
    t.step({0x05}, 40);
    t.step({0x7F}, 40);
    t.settle();
    check(old_ok && new_ok && t.ended({kOk}), "raw lane: an unnamed TYPE byte");
  }
  {
    GwgTwin t;
    const uint8_t bytes[] = {0x01, 0xCB, 0x55, 0x01, 0x04};
    const bool old_ok = t.old_engine.frame(bytes, sizeof(bytes), 0);
    const bool new_ok = t.engine.submit(optolink::make_frame(bytes, sizeof(bytes), 0));
    t.step({0x05}, 40);
    t.step({0x42, 0x43}, 40);
    t.settle();
    check(old_ok && new_ok && t.ended({kOk}), "open frame, collected until quiet");
  }
  {
    GwgTwin t;
    check(!t.engine.submit(optolink::make_request(optolink::Lane::P300, 0x01, 0x55, 1, nullptr)) &&
              !t.engine.submit(optolink::make_request(optolink::Lane::KW, 0xF7, 0x55, 1, nullptr)),
          "the GWG adapter refuses the P300 and KW lanes");
    check(!t.engine.submit(optolink::make_frame(nullptr, 3, 1)), "and an open frame without bytes");
  }
}

#else

using P300Twin = Twin<RefVs2>;

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

// The link up (the EOT, the device's 05, our 16 00 00, its 06), then the old
// call and the hub's request side by side, then `script` once the request is
// out.
template<class OldCall>
P300Twin &p300_run(P300Twin &t, bool built, const optolink::Request &r, OldCall old_call,
                   const std::vector<Bytes> &script, bool &alike) {
  t.step({}, 8);
  t.step({0x05}, 8);
  t.step({0x06}, 8);
  const bool old_ok = old_call(t.old_engine);
  const bool new_ok = built && t.engine.submit(r);
  // The request goes out before anything answers it.
  t.step({}, 8);
  for (const Bytes &b : script)
    t.step(b, 8);
  t.settle();
  alike = old_ok == new_ok;
  return t;
}

template<class OldCall>
void p300_case(const char *what, const Statuses &want, bool built, const optolink::Request &r, OldCall old_call,
               const std::vector<Bytes> &script) {
  P300Twin t;
  bool alike = false;
  p300_run(t, built, r, old_call, script, alike);
  check(alike && t.ended(want), what);
}

void p300_cases() {
  std::printf("-- P300 --\n");
  optolink::Request r{};
  const uint8_t data[] = {0x03, 0x04};
  const uint8_t param[] = {0x01, 0x02};

  {
    P300Twin t;
    bool alike = false;
    const bool b = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
    p300_run(
        t, b, r, [](RefVs2 &e) { return e.read(0x00F8, 2); },
        {acked(p300({0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB}))}, alike);
    check(alike && t.ended({kOk}) && t.new_events[0].data == Bytes({0x20, 0xCB}), "OWN read: same bytes, same result");
  }
  {
    P300Twin t;
    bool alike = false;
    const bool b = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
    p300_run(
        t, b, r, [](RefVs2 &e) { return e.read(0x00F8, 2); }, {acked(p300({0x06, 0x03, 0x01, 0x00, 0xF8, 0x02, 0x05}))},
        alike);
    check(alike && t.ended({OptolinkResult::DEVICE_ERROR}) && t.new_events[0].error_code == Bytes({0x05}),
          "an error frame: DEVICE_ERROR with its code 05");
  }
  bool b = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
  p300_case("no answer: TIMEOUT", {kTimeout}, b, r, [](RefVs2 &e) { return e.read(0x00F8, 2); }, {});
  b = read_request(AddressSpace::OWN, 0x2000, optolink::MAX_PAYLOAD + 1, nullptr, 0, r);
  p300_case("a read past MAX_PAYLOAD: both refuse", {}, b, r,
            [](RefVs2 &e) { return e.read(0x2000, optolink::MAX_PAYLOAD + 1); }, {});

  b = write_request(AddressSpace::OWN, 0x2306, data, 2, nullptr, 0, r);
  p300_case("OWN write", {kOk}, b, r, [&data](RefVs2 &e) { return e.write(0x2306, data, 2); },
            {acked(p300({0x05, 0x01, 0x02, 0x23, 0x06, 0x02}))});

  b = read_request(AddressSpace::RPC, 0x1234, 4, param, 2, r);
  p300_case("procedure call with its parameter", {kOk}, b, r, [&param](RefVs2 &e) { return e.rpc(0x1234, param, 2); },
            {acked(p300({0x07, 0x01, 0x07, 0x12, 0x34, 0x02, 0xAA, 0xBB}))});
  b = write_request(AddressSpace::RPC, 0x1234, data, 2, nullptr, 0, r);
  p300_case("an action button: the call without parameter", {kOk}, b, r,
            [](RefVs2 &e) { return e.rpc(0x1234, nullptr, 0); }, {acked(p300({0x05, 0x01, 0x07, 0x12, 0x34, 0x00}))});

  {
    P300Twin t;
    bool alike = false;
    const bool built = read_request(AddressSpace::GFA, 0x4050, 1, nullptr, 0, r);
    p300_run(
        t, built, r, [](RefVs2 &e) { return e.gfaRead(0x4050, 1); }, {{}, {0x05}, {0x24}, {}, {0x05}, {0x06}}, alike);
    check(alike && t.ended({kOk}) && t.new_events[0].data == Bytes({0x24}), "GFA read: the KW detour");
  }

  r = optolink::make_request(optolink::Lane::P300, 0x05, 0x1000, 2, nullptr);
  p300_case("raw lane: a generic function code", {kOk}, true, r, [](RefVs2 &e) { return e.request(0x05, 0x1000, 2); },
            {acked(p300({0x07, 0x01, 0x05, 0x10, 0x00, 0x02, 0xAA, 0xBB}))});
  r = optolink::make_request(optolink::Lane::KW, 0xF4, 0x2306, 1, data);
  p300_case("raw lane: a KW telegram with payload", {kOk}, true, r,
            [&data](RefVs2 &e) { return e.kwTelegram(0xF4, 0x2306, 1, data); },
            {{}, {0x05}, {0x00}, {}, {0x05}, {0x06}});
  const uint8_t frame[] = {0x16, 0x00, 0x00};
  r = optolink::make_frame(frame, sizeof(frame), 1);
  p300_case("open frame", {kOk}, true, r, [&frame](RefVs2 &e) { return e.frame(frame, sizeof(frame), 1); }, {{0x06}});

  {
    // A read right after a restart: its watchdog runs from the restart.
    P300Twin t;
    t.step({}, 8);
    t.park(3500);
    const bool b2 = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, r);
    const bool old_ok = t.old_engine.read(0x00F8, 2);
    const bool new_ok = b2 && t.engine.submit(r);
    for (int i = 0; i < 8; ++i)
      t.step({}, 100);  // 800 ms before the device's ENQ
    t.step({0x05}, 8);
    t.step({0x06}, 8);
    t.step({}, 8);
    t.step(acked(p300({0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB})), 8);
    t.settle();
    check(old_ok && new_ok && t.ended({kOk}), "after a 3.5 s park: no early TIMEOUT, answered");
  }

  P300Twin t;
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
#elif defined(VITOHOME_PROTOCOL_GWG)
  gwg_cases();
#else
  p300_cases();
#endif
  std::printf("hub requests: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
