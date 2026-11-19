// Proof for raw_script.h: the runner of a timed open frame, on a scripted
// port and the frozen host clock.
//
//   A) the #35 A/B/C shapes: a leading wait counts from the ENQ, a wait inside
//      the frame from the previous write, and the 01 of C goes out in the pass
//      that read the ENQ;
//   B) KW/GWG wait for the ENQ: other bytes before it are counted, none goes
//      out without it, and none arrives too late (NO_ENQ); @now sends at once;
//   C) the answer: n bytes, until quiet, a short answer is a TIMEOUT, and the
//      silence check runs before the drain;
//   D) / p300: ACK and frame with a good checksum, a bad one, a NACK, and
//      something else after the ACK;
//   E) bytes during a wait are not the answer; the whole-script deadline;
//   F) the result line.
//
// Built by build_and_run.sh.

#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <vector>

#include "raw_script.h"

using namespace esphome::vitohome;
using esphome::vitohome::optolink::optolink_test_clock_advance;
using esphome::vitohome::optolink::optolink_test_clock_freeze;
using esphome::vitohome::optolink::optolink_test_clock_release;

namespace {
int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

struct TimedPort {
  struct Write {
    uint32_t t;
    std::vector<uint8_t> bytes;
  };
  std::deque<uint8_t> in;
  std::vector<Write> writes;

  std::size_t available() const { return this->in.size(); }
  uint8_t read() {
    const uint8_t b = this->in.front();
    this->in.pop_front();
    return b;
  }
  std::size_t write(const uint8_t *data, uint8_t length) {
    this->writes.push_back({optolink_millis(), std::vector<uint8_t>(data, data + length)});
    return length;
  }
  void feed(std::initializer_list<uint8_t> bytes) { this->in.insert(this->in.end(), bytes.begin(), bytes.end()); }
};

constexpr RawScriptTiming kGwg{3000, 250, 6000};
constexpr RawScriptTiming kKw{3000, 1000, 6000};
constexpr RawScriptTiming kP300{3000, 500, 6000};

RawSpec spec_of(const char *text) {
  RawSpec spec{};
  const char *error = nullptr;
  if (!parse_raw_spec(text, spec, error, 48)) {
    std::printf("  spec '%s' refused: %s\n", text, error);
    ++g_fail;
  }
  return spec;
}

bool bytes_are(const std::vector<uint8_t> &got, std::initializer_list<uint8_t> want) {
  return got.size() == want.size() && std::equal(got.begin(), got.end(), want.begin());
}

void advance(uint32_t ms) { optolink_test_clock_advance(ms); }
}  // namespace

int main() {
  std::printf("== raw_script.h: timed open frames ==\n");
  optolink_test_clock_freeze();

  // --- A: the #35 shapes ----------------------------------------------------
  {
    // B: 30 ms after the ENQ, then the whole frame.
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f ~30 01 AE 16 01 04 / 2"), kGwg);
    check(!script.loop() && port.writes.empty(), "A: nothing goes out before the ENQ");
    advance(400);
    port.feed({0x05});
    const uint32_t enq = optolink_millis();
    check(!script.loop() && port.writes.empty(), "A: B waits after the ENQ");
    advance(29);
    check(!script.loop() && port.writes.empty(), "A: B still waits at 29 ms");
    advance(1);
    check(!script.loop() && port.writes.size() == 1 && port.writes[0].t - enq == 30 &&
              bytes_are(port.writes[0].bytes, {0x01, 0xAE, 0x16, 0x01, 0x04}),
          "A: B writes the frame 30 ms after the ENQ, in one write");
    advance(40);
    port.feed({0xE9, 0x16});
    check(script.loop() && script.result().status == RawScriptStatus::OK && script.result().answer_len == 2 &&
              script.result().answer[0] == 0xE9 && script.result().answer_after_ms == 40,
          "A: B's answer, 40 ms after the write");
    check(script.result().enq_seen && script.result().enq_ms == 400, "A: the ENQ came 400 ms after the start");
  }
  {
    // C: the 01 on the ENQ, the rest 30 ms later.
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f 01 ~30 AE 16 01 04 / 2"), kGwg);
    port.feed({0x05});
    const uint32_t enq = optolink_millis();
    check(
        !script.loop() && port.writes.size() == 1 && port.writes[0].t == enq && bytes_are(port.writes[0].bytes, {0x01}),
        "A: C's 01 goes out in the pass that read the ENQ");
    advance(30);
    check(!script.loop() && port.writes.size() == 2 && port.writes[1].t - enq == 30 &&
              bytes_are(port.writes[1].bytes, {0xAE, 0x16, 0x01, 0x04}),
          "A: C's rest 30 ms later");
    check(script.result().segments == 2 && script.result().sent_at_ms[0] == 0 && script.result().sent_at_ms[1] == 30,
          "A: both segments recorded with their times");
  }
  {
    // Two waits: each counts from the previous write.
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("kw f 01 ~20 F7 00 ~10 F8 02 / 2"), kKw);
    port.feed({0x05});
    script.loop();
    advance(20);
    script.loop();
    advance(9);
    script.loop();
    check(port.writes.size() == 2, "A: the second wait counts from the second write");
    advance(1);
    script.loop();
    check(
        port.writes.size() == 3 && bytes_are(port.writes[2].bytes, {0xF8, 0x02}) && script.result().sent_at_ms[2] == 30,
        "A: the third segment at +30 ms");
  }

  // --- B: the ENQ ---------------------------------------------------------
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f ~5 01 CB 6F 01 04 / 1"), kGwg);
    port.feed({0x00, 0xFF});
    script.loop();
    advance(3001);
    check(script.loop() && script.result().status == RawScriptStatus::NO_ENQ && port.writes.empty() &&
              script.result().noise_count == 2,
          "B: no ENQ within 3000 ms: nothing sent, two other bytes counted");
  }
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("kw f @now 01 F7 00 F8 02 / 2"), kKw);
    check(!script.loop() && port.writes.size() == 1 && bytes_are(port.writes[0].bytes, {0x01, 0xF7, 0x00, 0xF8, 0x02}),
          "B: @now sends in the first pass, without an ENQ");
  }
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f @now ~15 01 CB 6F 01 04 / 1"), kGwg);
    script.loop();
    advance(15);
    script.loop();
    check(port.writes.size() == 1 && port.writes[0].t - (optolink_millis() - 15) == 15,
          "B: @now with a leading wait counts from the start");
  }

  // --- C: the answer ------------------------------------------------------
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f ~5 01 CB 6F 04"), kGwg);
    port.feed({0x05});
    script.loop();
    advance(5);
    script.loop();
    advance(20);
    port.feed({0x11, 0x22, 0x33});
    check(!script.loop(), "C: collecting until quiet goes on after three bytes");
    advance(251);
    check(script.loop() && script.result().status == RawScriptStatus::OK && script.result().answer_len == 3,
          "C: the quiet line ends the answer, three bytes");
  }
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f ~5 01 CB 6F 01 04 / 2"), kGwg);
    port.feed({0x05});
    script.loop();
    advance(5);
    script.loop();
    advance(10);
    port.feed({0x11});
    script.loop();
    advance(251);
    check(script.loop() && script.result().status == RawScriptStatus::TIMEOUT && script.result().answer_len == 1,
          "C: one of two bytes, then silence: TIMEOUT with the byte kept");
  }
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f ~5 01 CB 6F 01 04 / 1"), kGwg);
    port.feed({0x05});
    script.loop();
    advance(5);
    script.loop();
    // The device's next ENQ sits in the buffer when the deadline has passed.
    advance(300);
    port.feed({0x05});
    check(script.loop() && script.result().status == RawScriptStatus::TIMEOUT && script.result().answer_total == 0,
          "C: a byte buffered past the deadline is not the answer");
  }

  // --- D: / p300 ------------------------------------------------------------
  {
    uint8_t frame[] = {0x41, 0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0x92, 0x00};
    frame[9] = p300_sum8(frame + 1, 8);
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("p300 f ~10 41 {05 00 01 00 F8 02} / p300"), kP300);
    check(!script.loop() && port.writes.empty(), "D: p300 waits 10 ms from the start");
    advance(10);
    script.loop();
    check(port.writes.size() == 1 && port.writes[0].bytes.size() == 8, "D: p300 writes the frame with its checksum");
    advance(20);
    port.feed({0x06});
    for (uint8_t b : frame)
      port.feed({b});
    check(script.loop() && script.result().status == RawScriptStatus::OK && script.result().answer_len == 11,
          "D: ACK and a frame with a good checksum");

    frame[9] ^= 0x01;
    script.start(spec_of("p300 f 41 {05 00 01 00 F8 02} / p300"), kP300);
    script.loop();
    port.feed({0x06});
    for (uint8_t b : frame)
      port.feed({b});
    check(script.loop() && script.result().status == RawScriptStatus::CHECKSUM, "D: a bad checksum is CHECKSUM");

    script.start(spec_of("p300 f 41 {05 00 01 00 F8 02} / p300"), kP300);
    script.loop();
    port.feed({0x15});
    check(script.loop() && script.result().status == RawScriptStatus::NACK, "D: a NACK ends it");

    script.start(spec_of("p300 f 41 {05 00 01 00 F8 02} / p300"), kP300);
    script.loop();
    port.feed({0x06, 0x05});
    check(script.loop() && script.result().status == RawScriptStatus::MALFORMED,
          "D: anything but 41 after the ACK is malformed");

    script.start(spec_of("p300 f 41 {05 00 01 00 F8 02} / p300"), kP300);
    script.loop();
    port.feed({0x41, 0xF0});
    check(script.loop() && script.result().status == RawScriptStatus::MALFORMED,
          "D: a frame longer than the answer buffer is malformed");
  }

  // --- E: waits and the deadline ------------------------------------------
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f 01 ~50 AE 16 01 04 / 2"), kGwg);
    port.feed({0x05});
    script.loop();
    advance(20);
    port.feed({0x05, 0x05});
    script.loop();
    advance(30);
    script.loop();
    advance(10);
    port.feed({0xE9, 0x16});
    check(script.loop() && script.result().status == RawScriptStatus::OK && script.result().dropped_count == 2 &&
              script.result().answer[0] == 0xE9,
          "E: two bytes during the wait are dropped, not the answer");
  }
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f ~5 01 CB 6F 01 04 / 1"), RawScriptTiming{3000, 250, 1000});
    advance(1001);
    check(script.loop() && script.result().status == RawScriptStatus::TIMEOUT && port.writes.empty(),
          "E: the whole-script deadline ends a script waiting for its ENQ");
  }

  // --- F: the result line -------------------------------------------------
  {
    TimedPort port;
    RawScript<TimedPort> script(&port);
    script.start(spec_of("gwg f 01 ~30 AE 16 01 04 / 2"), kGwg);
    advance(812);
    port.feed({0x05});
    script.loop();
    advance(30);
    script.loop();
    advance(40);
    port.feed({0xE9, 0x16});
    script.loop();
    char line[256];
    format_raw_script(script.spec(), script.result(), kGwg, line, sizeof(line));
    const char *want = "gwg f 01 ~30 AE 16 01 04 / 2: ENQ after 812 ms, sent 01 +0 ms, AE 16 01 04 +30 ms; answer +40 "
                       "ms: E9 16";
    check(std::strcmp(line, want) == 0, "F: the result line");
    if (std::strcmp(line, want) != 0)
      std::printf("    got:  %s\n    want: %s\n", line, want);

    script.start(spec_of("gwg f ~5 01 CB 6F 01 04 / 1"), kGwg);
    advance(3001);
    script.loop();
    format_raw_script(script.spec(), script.result(), kGwg, line, sizeof(line));
    check(std::strcmp(line, "gwg f ~5 01 CB 6F 01 04 / 1: no ENQ within 3000 ms") == 0, "F: the line without an ENQ");

    char small[16];
    format_raw_script(script.spec(), script.result(), kGwg, small, sizeof(small));
    check(std::strlen(small) == sizeof(small) - 1, "F: a short buffer is filled and terminated");
  }

  optolink_test_clock_release();
  std::printf("raw script: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
