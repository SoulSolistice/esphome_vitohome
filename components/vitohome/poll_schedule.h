#pragma once
#include <cstdint>

namespace esphome::vitohome {

// Pure scheduling arithmetic for per-entity poll intervals, split out of
// VitoHomeComponent::schedule_due_entities_() so it can be proven on the host
// (tests/native/proof_scheduler.cpp) without millis() or an entity vector.
//
// All time values are millis()-style uint32_t and wrap after ~49.7 days; every
// comparison goes through a signed difference, which is wrap-correct as long as
// the two values are less than 2^31 ms (~24.8 days) apart.
//
// `next_due_ms == 0` is the "never polled" sentinel: the entity is due
// immediately. Because 0 is meaningful, a computed next-due of exactly 0 is
// nudged to 1 -- a one-millisecond error once every 49.7 days, versus a spurious
// "never polled" flag.
struct PollDecision {
  bool due;
  uint32_t next_due_ms;
};

inline PollDecision poll_schedule_step(uint32_t now, uint32_t next_due_ms, uint32_t interval_ms, uint32_t slack_ms) {
  if (interval_ms == 0) {
    // 0 = "poll on every hub cycle"; nothing to schedule.
    return {true, 0};
  }
  if (next_due_ms != 0 && static_cast<int32_t>((now + slack_ms) - next_due_ms) < 0) {
    return {false, next_due_ms};
  }
  // Advance from the previous SCHEDULED time, not from `now`. Anchoring on
  // `now` (which is sampled inside the hub's update() callback, a few ms after
  // the interval tick that invoked it) pushed the next due time just past the
  // next tick whenever interval == hub tick, making the poll a coin flip
  // decided by loop jitter -- hardware-observed on VScotHO1_72, where the same
  // firmware binary dropped the whole 60 s tier on tick 2 in one boot and never
  // dropped it in another. It also let jitter accumulate into the period.
  uint32_t next = (next_due_ms == 0 ? now : next_due_ms) + interval_ms;
  // Fell a whole period behind (long bus stall, or a first poll): re-anchor on
  // `now` rather than replay a backlog of missed slots.
  if (static_cast<int32_t>(next - now) <= 0) next = now + interval_ms;
  if (next == 0) next = 1;  // 0 is the "never polled" sentinel
  return {true, next};
}

}  // namespace esphome::vitohome
