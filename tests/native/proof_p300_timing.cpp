// The P300 exchange's timing points (optolink_p300.h, TimingPoint), on a
// frozen clock advanced 1 ms per pass:
//   (1) ENQ_TO_SYNC: our 16 00 00 goes out that long after the device's ENQ,
//       not a pass earlier; at 0 in the pass after it; the keepalive's SYNC,
//       which answers no ENQ, is not held;
//   (2) FRAME_TO_ACK: our 06 that long after the answer frame; the result is
//       delivered with the frame, not with the ACK;
//   (3) ANSWER_TO_NEXT: the next telegram that long after our 06;
//   (4) ENQ_TO_SEND: on the KW detour, the 01 and the telegram that long
//       after the device's ENQ, bytes meanwhile dropped; at 0 in the ENQ's
//       pass;
//   (5) the loop runs fast while a write waits, and not otherwise;
//   (6) each point is clamped to its bound; the KW exchange refuses the two
//       P300 points.
// 0 for every point is the pacing without them: the differential target
// holds that to the recorded traces.
//
// Built by build_and_run_protocols.sh.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"

using namespace esphome::vitohome;
using Engine = optolink::P300Exchange<FakeOptolink>;
using optolink::TimingPoint;
using Bytes = std::vector<uint8_t>;

namespace {

int g_fail = 0;
int g_responses = 0;
int g_errors = 0;
Bytes g_data;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

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

const Bytes kSync{0x16, 0x00, 0x00};
const Bytes kRead = p300({0x05, 0x00, 0x01, 0x00, 0xF8, 0x02});
const Bytes kAnswer = p300({0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB});

struct Rig {
  FakeOptolink io;
  Engine e{&io};
  Rig() {
    kw_calls::wire(
        e,
        [](const uint8_t *d, uint8_t n, uint16_t) {
          ++g_responses;
          g_data.assign(d, d + n);
        },
        [](optolink::OptolinkResult, uint16_t) { ++g_errors; });
    e.begin();
  }
  void set(TimingPoint point, uint32_t ms) { e.set_timing(point, ms); }
  // One pass, then 1 ms.
  void pass() {
    e.loop();
    optolink::optolink_test_clock_advance(1);
  }
  // Passes until `want` is written or `max` passes; the passes it took.
  int until(const Bytes &want, int max) {
    for (int i = 1; i <= max; ++i) {
      this->pass();
      if (io.written() == want)
        return i;
    }
    return -1;
  }
  // The EOT, the device's ENQ, our SYNC, its ACK: in IDLE afterwards.
  void session() {
    this->pass();
    io.feed({0x05});
    this->pass();
    this->pass();
    io.feed({0x06});
    this->pass();
    io.clear_written();
  }
};

void enq_to_sync() {
  std::printf("-- (1) ENQ_TO_SYNC --\n");
  {
    Rig r;
    r.pass();
    r.io.clear_written();
    r.io.feed({0x05});
    check(r.until(kSync, 5) == 2, "0: SYNC in the pass after the ENQ's");
  }
  {
    Rig r;
    r.set(TimingPoint::ENQ_TO_SYNC, 30);
    r.pass();
    r.io.clear_written();
    r.io.feed({0x05});
    r.pass();  // the ENQ read at t0
    bool early = false;
    for (int i = 1; i < 30; ++i) {
      r.pass();
      early = early || !r.io.written().empty();
    }
    check(!early, "30 ms: nothing before 30 ms after the ENQ");
    check(r.e.wants_fast_loop(), "30 ms: the loop runs fast meanwhile");
    r.pass();
    check(r.io.written() == kSync, "30 ms: SYNC at 30 ms");
    check(!r.e.wants_fast_loop(), "30 ms: and the fast loop ends");
  }
  {
    Rig r;
    r.set(TimingPoint::ENQ_TO_SYNC, 100);
    r.pass();
    r.io.feed({0x05});
    for (int i = 0; i < 101; ++i)
      r.pass();
    r.io.feed({0x06});
    r.pass();
    r.io.clear_written();
    // Idle past the keepalive: its SYNC answers no ENQ, so it goes out in
    // the pass after the keepalive's, as at 0.
    optolink::optolink_test_clock_advance(optolink::P300_KEEPALIVE_MS);
    check(r.until(kSync, 5) == 2, "the keepalive's SYNC is not held");
  }
}

void frame_to_ack() {
  std::printf("-- (2) FRAME_TO_ACK --\n");
  {
    Rig r;
    r.session();
    kw_calls::read(r.e, 0x00F8, 2);
    r.pass();
    r.io.clear_written();
    r.io.feed({0x06});
    r.io.feed(kAnswer);
    r.pass();  // the ACK of the request
    r.pass();  // the frame
    check(r.io.written().empty() && g_responses == 1, "0: answer delivered with the frame");
    r.pass();
    check(r.io.written() == Bytes({0x06}), "0: our 06 in the next pass");
  }
  {
    Rig r;
    r.set(TimingPoint::FRAME_TO_ACK, 40);
    r.session();
    const int responses = g_responses;
    kw_calls::read(r.e, 0x00F8, 2);
    r.pass();
    r.io.clear_written();
    r.io.feed({0x06});
    r.io.feed(kAnswer);
    r.pass();
    r.pass();  // the frame at t0
    check(g_responses == responses + 1, "40 ms: delivered with the frame");
    bool early = false;
    for (int i = 1; i < 40; ++i) {
      r.pass();
      early = early || !r.io.written().empty();
    }
    check(!early && r.e.wants_fast_loop(), "40 ms: no 06 before 40 ms, the loop fast");
    r.pass();
    check(r.io.written() == Bytes({0x06}), "40 ms: our 06 at 40 ms");
  }
}

void answer_to_next() {
  std::printf("-- (3) ANSWER_TO_NEXT --\n");
  for (const uint32_t ms : {0u, 50u}) {
    Rig r;
    r.set(TimingPoint::ANSWER_TO_NEXT, ms);
    r.session();
    kw_calls::read(r.e, 0x00F8, 2);
    r.pass();
    r.io.feed({0x06});
    r.io.feed(kAnswer);
    r.pass();
    r.pass();
    r.pass();  // our 06 at t0
    r.io.clear_written();
    check(!r.e.wants_fast_loop(),
          ms == 0 ? "0: nothing waits without a request" : "50 ms: nothing waits without a request");
    kw_calls::read(r.e, 0x00F8, 2);
    const int took = r.until(kRead, 100);
    if (ms == 0) {
      check(took == 1, "0: the next read in the next pass");
    } else {
      check(took == 50, "50 ms: the next read 50 ms after our 06");
    }
  }
  {
    Rig r;
    r.set(TimingPoint::ANSWER_TO_NEXT, 50);
    r.session();
    kw_calls::read(r.e, 0x00F8, 2);
    r.pass();
    r.io.feed({0x06});
    r.io.feed(kAnswer);
    r.pass();
    r.pass();
    r.pass();
    kw_calls::read(r.e, 0x00F8, 2);
    r.pass();
    check(r.e.wants_fast_loop(), "50 ms: a request waiting runs the loop fast");
  }
}

void enq_to_send() {
  std::printf("-- (4) ENQ_TO_SEND on the KW detour --\n");
  const Bytes gfa{0x01, 0x6B, 0x40, 0x50, 0x01};
  {
    Rig r;
    r.session();
    kw_calls::gfa_read(r.e, 0x4050, 1);
    r.pass();
    r.pass();  // the EOT
    r.io.clear_written();
    r.io.feed({0x06, 0x05});
    r.pass();
    r.pass();
    check(r.io.written() == gfa, "0: 01 6B 40 50 01 in the ENQ's pass");
  }
  {
    Rig r;
    r.set(TimingPoint::ENQ_TO_SEND, 20);
    r.session();
    const int responses = g_responses;
    kw_calls::gfa_read(r.e, 0x4050, 1);
    r.pass();
    r.pass();
    r.io.clear_written();
    r.io.feed({0x06});
    r.pass();
    r.io.feed({0x05});
    r.pass();  // the ENQ at t0
    check(r.e.wants_fast_loop(), "20 ms: the loop runs fast");
    r.io.feed({0x05});  // a byte meanwhile: no answer
    bool early = false;
    for (int i = 1; i < 20; ++i) {
      r.pass();
      early = early || !r.io.written().empty();
    }
    check(!early, "20 ms: nothing before 20 ms");
    r.pass();
    check(r.io.written() == gfa, "20 ms: 01 6B 40 50 01 at 20 ms");
    r.pass();
    check(g_responses == responses, "20 ms: the byte meanwhile was dropped");
    r.io.feed({0x24});
    r.pass();
    check(g_responses == responses + 1 && g_data == Bytes({0x24}), "20 ms: the answer after the telegram");
  }
}

void bounds() {
  std::printf("-- (6) bounds --\n");
  FakeOptolink io;
  Engine e(&io);
  const TimingPoint points[] = {TimingPoint::ENQ_TO_SEND, TimingPoint::ANSWER_TO_NEXT, TimingPoint::ENQ_TO_SYNC,
                                TimingPoint::FRAME_TO_ACK};
  bool clamped = true;
  for (TimingPoint point : points) {
    uint32_t ms = 10000;
    clamped = clamped && e.set_timing(point, ms) && ms == optolink::timing_max(point) &&
              e.timing(point) == optolink::timing_max(point) && e.timing_default(point) == 0;
  }
  check(clamped, "every point clamped to its bound, default 0");
  optolink::KwExchange<FakeOptolink> kw(&io);
  uint32_t ms = 10;
  check(!kw.set_timing(TimingPoint::ENQ_TO_SYNC, ms) && !kw.set_timing(TimingPoint::FRAME_TO_ACK, ms) &&
            kw.timing(TimingPoint::ENQ_TO_SYNC) == 0 && kw.timing(TimingPoint::FRAME_TO_ACK) == 0,
        "the KW exchange refuses ENQ_TO_SYNC and FRAME_TO_ACK");
}

}  // namespace

int main() {
  std::printf("== P300 link timing ==\n");
  optolink::optolink_test_clock_freeze();
  enq_to_sync();
  frame_to_ack();
  answer_to_next();
  enq_to_send();
  bounds();
  std::printf("P300 link timing: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
