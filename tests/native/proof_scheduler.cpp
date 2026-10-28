// Host proof for components/vitohome/poll_schedule.h.
//
// The bug this pins: an entity whose update_interval equals the hub tick was
// polled or skipped depending on loop jitter, because the next due time was
// re-anchored on a `now` sampled a few ms after the tick that scheduled it.
// Two 2026-07-09 hardware logs from the SAME firmware binary disagreed on
// whether the whole 60 s tier fired on the second tick.
//
// Build: g++ -std=c++17 -Wall -Wextra -I<component root> proof_scheduler.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "poll_schedule.h"

using esphome::vitohome::poll_schedule_step;
using esphome::vitohome::PollDecision;

namespace {

constexpr uint32_t HUB_TICK = 60000;
constexpr uint32_t SLACK = HUB_TICK / 2;

// interval == 0 means "every hub cycle", never scheduled.
void test_zero_interval_always_due() {
  for (uint32_t now : {0u, 1u, 123456u}) {
    PollDecision d = poll_schedule_step(now, 0, 0, SLACK);
    assert(d.due);
    assert(d.next_due_ms == 0);
  }
  std::puts("ok: interval 0 -> due on every cycle");
}

// next_due == 0 is the "never polled" sentinel: due immediately, and the next
// due time is anchored on `now`.
void test_first_poll_is_due() {
  PollDecision d = poll_schedule_step(5000, 0, HUB_TICK, SLACK);
  assert(d.due);
  assert(d.next_due_ms == 5000 + HUB_TICK);
  std::puts("ok: never-polled entity is due immediately");
}

// THE REGRESSION. ESPHome fires update() at T_k and `now = millis()` is sampled
// a few ms later; that offset is NOT constant. Anchoring next_due on `now` made
// the poll fire only when this tick's offset >= the previous tick's.
void test_old_formula_skips_under_realistic_jitter() {
  static const uint32_t offset[12] = {0, 4, 1, 7, 2, 9, 0, 3, 6, 1, 8, 2};
  uint32_t next_due = 0;
  int skipped_old = 0;
  for (int k = 0; k < 12; k++) {
    const uint32_t now = static_cast<uint32_t>(k) * HUB_TICK + offset[k];
    if (next_due != 0 && static_cast<int32_t>(now - next_due) < 0) {
      skipped_old++;
      continue;
    }
    next_due = now + HUB_TICK;  // the old, broken advance
  }
  assert(skipped_old > 0);  // the bug is real

  next_due = 0;
  int skipped_new = 0;
  for (int k = 0; k < 12; k++) {
    const uint32_t now = static_cast<uint32_t>(k) * HUB_TICK + offset[k];
    PollDecision d = poll_schedule_step(now, next_due, HUB_TICK, SLACK);
    if (!d.due) {
      skipped_new++;
      continue;
    }
    next_due = d.next_due_ms;
  }
  assert(skipped_new == 0);
  std::printf("ok: same jitter train -- old skipped %d/12 ticks, new skipped %d/12\n", skipped_old, skipped_new);
}

// interval == hub tick must fire on EVERY tick, for any jitter, with no creep.
void test_interval_equal_to_hub_tick_fires_every_tick() {
  for (uint32_t jitter : {0u, 3u, 17u, 250u, 1500u}) {
    uint32_t next_due = 0;
    uint32_t first_due = 0;
    for (int tick = 0; tick < 240; tick++) {  // four simulated hours
      const uint32_t now = 5000 + static_cast<uint32_t>(tick) * HUB_TICK + (tick == 0 ? 0 : jitter);
      PollDecision d = poll_schedule_step(now, next_due, HUB_TICK, SLACK);
      if (!d.due) {
        std::printf("FAIL: jitter=%u tick=%d skipped\n", jitter, tick);
        assert(false);
      }
      next_due = d.next_due_ms;
      if (tick == 0)
        first_due = next_due;
    }
    // No creep: after N polls the schedule is exactly first_due + (N-1)*interval.
    assert(next_due == first_due + 239u * HUB_TICK);
  }
  std::puts("ok: interval == hub tick fires on every tick, no drift");
}

// A slow tier still skips ticks, and lands on the right ones.
void test_slow_tier_skips_ticks() {
  const uint32_t interval = 300000;  // 300 s = 5 hub ticks
  uint32_t next_due = 0;
  int fired = 0;
  for (int tick = 0; tick < 50; tick++) {
    const uint32_t now = 5000 + static_cast<uint32_t>(tick) * HUB_TICK;
    PollDecision d = poll_schedule_step(now, next_due, interval, SLACK);
    if (d.due) {
      fired++;
      next_due = d.next_due_ms;
    }
  }
  // Tick 0 (never polled) plus one every 5 ticks thereafter.
  assert(fired == 10);
  std::puts("ok: 300 s tier fires once per 5 hub ticks");
}

// A long stall must re-anchor rather than replay every missed slot.
void test_long_stall_reanchors() {
  const uint32_t interval = 60000;
  PollDecision d = poll_schedule_step(1000, 0, interval, SLACK);
  assert(d.due && d.next_due_ms == 61000);
  // Bus wedged for an hour: schedule one period out from NOW, not from a due
  // time that is already deep in the past.
  d = poll_schedule_step(3661000, d.next_due_ms, interval, SLACK);
  assert(d.due);
  assert(d.next_due_ms == 3661000 + interval);
  std::puts("ok: a long stall re-anchors instead of replaying missed slots");
}

// millis() wraps at 2^32. The signed-difference compare must survive it, and a
// computed next-due of exactly 0 must be nudged off the "never polled" sentinel.
void test_wraparound() {
  const uint32_t interval = 60000;
  const uint32_t now = 0xFFFFFFFFu - 1000u;  // 1 s before the wrap
  PollDecision d = poll_schedule_step(now, 0, interval, SLACK);
  assert(d.due);
  const uint32_t next = d.next_due_ms;  // wrapped past zero
  assert(next == static_cast<uint32_t>(now + interval));
  const uint32_t just_before = static_cast<uint32_t>(next - SLACK - 1u);
  assert(!poll_schedule_step(just_before, next, interval, SLACK).due);
  assert(poll_schedule_step(static_cast<uint32_t>(next - SLACK), next, interval, SLACK).due);

  // A next-due landing exactly on the sentinel is nudged to 1.
  const uint32_t sentinel_now = static_cast<uint32_t>(0u - interval);
  PollDecision s = poll_schedule_step(sentinel_now, 0, interval, SLACK);
  assert(s.due);
  assert(s.next_due_ms == 1);
  std::puts("ok: wrap-safe, and never lands on the never-polled sentinel");
}

// The slack window must not let an entity fire twice within one interval.
void test_slack_does_not_double_fire() {
  const uint32_t interval = 60000;
  PollDecision d = poll_schedule_step(0, 0, interval, SLACK);
  assert(d.due && d.next_due_ms == interval);
  assert(!poll_schedule_step(29999, d.next_due_ms, interval, SLACK).due);
  // At the slack edge: fire half a tick early rather than a whole tick late.
  assert(poll_schedule_step(30000, d.next_due_ms, interval, SLACK).due);
  std::puts("ok: slack fires at most half a hub tick early");
}

}  // namespace

// `update_interval: never` (ESPHome's SCHEDULER_DONT_RUN, 0xFFFFFFFF) is never
// due -- not on the first cycle, not ever. As an ordinary interval it put the
// next due time 1 ms in the past and polled on every tick (external review,
// 2026-10-28).
void test_never_is_never_due() {
  using esphome::vitohome::POLL_NEVER;
  for (uint32_t now : {0u, 1u, 60000u, 123456789u, 0xFFFFFFF0u}) {
    PollDecision d = poll_schedule_step(now, 0, POLL_NEVER, SLACK);
    assert(!d.due);
    d = poll_schedule_step(now, now + 1, POLL_NEVER, SLACK);
    assert(!d.due);
  }
  std::puts("ok: never -> never due");
}

// Every interval the config accepts (up to POLL_INTERVAL_MAX_MS, 24.8 days)
// schedules forward: after a poll the entity is not due again a tick later,
// and is due once the interval has passed -- from any start, across the wrap.
void test_every_accepted_interval_schedules_forward() {
  using esphome::vitohome::POLL_INTERVAL_MAX_MS;
  for (uint32_t interval : {2 * HUB_TICK, 3600000u, 86400000u, 864000000u, 2073600000u, POLL_INTERVAL_MAX_MS}) {
    for (uint32_t start : {0u, 7u, 0x7FFFFFFFu, 0xFFFF0000u}) {
      PollDecision first = poll_schedule_step(start, 0, interval, SLACK);
      assert(first.due);
      PollDecision next_tick = poll_schedule_step(start + HUB_TICK, first.next_due_ms, interval, SLACK);
      assert(!next_tick.due);
      PollDecision later = poll_schedule_step(start + interval, first.next_due_ms, interval, SLACK);
      assert(later.due);
    }
  }
  std::puts("ok: intervals up to 2^31-1 ms schedule forward");
}

int main() {
  test_zero_interval_always_due();
  test_first_poll_is_due();
  test_old_formula_skips_under_realistic_jitter();
  test_interval_equal_to_hub_tick_fires_every_tick();
  test_slow_tier_skips_ticks();
  test_long_stall_reanchors();
  test_wraparound();
  test_slack_does_not_double_fire();
  test_never_is_never_due();
  test_every_accepted_interval_schedules_forward();
  std::puts("proof_scheduler: all checks passed");
  return 0;
}
