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
// `update_interval: never` arrives as ESPHome's SCHEDULER_DONT_RUN, the
// largest uint32_t: never due. Treated as an ordinary interval it put the next
// due time 1 ms in the past -- polled on EVERY hub tick. Every other interval
// must stay below 2^31 ms for the signed difference above; the config
// validation (POLL_INTERVAL_MAX_MS in __init__.py) rejects longer ones.
inline constexpr uint32_t POLL_NEVER = 0xFFFFFFFFu;
inline constexpr uint32_t POLL_INTERVAL_MAX_MS = 0x7FFFFFFFu;

struct PollDecision {
  bool due;
  uint32_t next_due_ms;
};

inline PollDecision poll_schedule_step(uint32_t now, uint32_t next_due_ms, uint32_t interval_ms, uint32_t slack_ms) {
  if (interval_ms == POLL_NEVER)
    return {false, next_due_ms};
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
  if (static_cast<int32_t>(next - now) <= 0)
    next = now + interval_ms;
  if (next == 0)
    next = 1;  // 0 is the "never polled" sentinel
  return {true, next};
}

// Poll backoff for a datapoint that keeps timing out while the link is alive
// (2026-11-08, issue #35: 54 `kmbus_eeprom` entities on a GWG unit that never
// answered them, each costing a 250 ms deadline plus an ENQ wait, every
// minute). `timeout_streak` counts consecutive TIMEOUTs that each followed
// some other answer on the link (VitoHomeComponent::note_poll_timeout_), so a
// dead link -- every entity timing out -- does not back anything off.
//
// From POLL_BACKOFF_AFTER timeouts on, the interval doubles per further
// timeout, up to POLL_BACKOFF_MAX_FACTOR: a 60 s entity is polled every 2, 4,
// 8, then 16 minutes, and on its old interval again after its first answer.
// It never stops: a datapoint that only answers in some burner phases still
// gets a chance. Interval 0 ("every hub cycle") and POLL_NEVER are left alone.
inline constexpr uint8_t POLL_BACKOFF_AFTER = 3;
inline constexpr uint32_t POLL_BACKOFF_MAX_FACTOR = 16;

inline uint32_t poll_backoff_interval(uint32_t interval_ms, uint8_t timeout_streak) {
  if (interval_ms == 0 || interval_ms == POLL_NEVER || timeout_streak < POLL_BACKOFF_AFTER)
    return interval_ms;
  const uint8_t doublings = static_cast<uint8_t>(timeout_streak - POLL_BACKOFF_AFTER + 1);
  uint32_t factor = 1;
  for (uint8_t i = 0; i < doublings && factor < POLL_BACKOFF_MAX_FACTOR; ++i)
    factor *= 2;
  // Saturate below POLL_INTERVAL_MAX_MS: the scheduler's signed differences
  // need intervals under 2^31 ms.
  if (interval_ms > POLL_INTERVAL_MAX_MS / factor)
    return POLL_INTERVAL_MAX_MS;
  return interval_ms * factor;
}

}  // namespace esphome::vitohome
