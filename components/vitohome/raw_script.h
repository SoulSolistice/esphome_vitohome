#pragma once

// The runner of a timed open frame (raw_op.h: `~ms`, `@now`, `/ p300`). It
// owns the port while the hub keeps the engine parked: it waits for the
// device's ENQ (KW, GWG), writes the frame in segments at their times and
// collects the answer. The hub restarts the engine afterwards, which resyncs
// the link. Header-only and ESPHome-free, like raw_op.h, so the native suite
// drives it against FakeOptolink (tests/native/proof_raw_script.cpp).

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "optolink_clock.h"
#include "raw_op.h"

namespace esphome::vitohome {

inline constexpr uint8_t RAW_SCRIPT_ANSWER_MAX = 64;
inline constexpr uint8_t RAW_SCRIPT_DROPPED_MAX = 8;
inline constexpr uint8_t RAW_SCRIPT_ENQ = 0x05;  // the device's sync byte (KW, GWG)

enum class RawScriptStatus : uint8_t { OK, NO_ENQ, TIMEOUT, NACK, CHECKSUM, MALFORMED };

inline const char *raw_script_status_name(RawScriptStatus status) {
  switch (status) {
    case RawScriptStatus::OK:
      return "ok";
    case RawScriptStatus::NO_ENQ:
      return "no ENQ";
    case RawScriptStatus::TIMEOUT:
      return "timeout";
    case RawScriptStatus::NACK:
      return "NACK";
    case RawScriptStatus::CHECKSUM:
      return "checksum wrong";
    default:
      return "malformed answer";
  }
}

struct RawScriptTiming {
  uint32_t enq_timeout_ms;  // KW, GWG: how long to wait for the device's ENQ
  uint32_t quiet_ms;        // silence before the first answer byte and between bytes
  uint32_t deadline_ms;     // the whole script
};

struct RawScriptResult {
  RawScriptStatus status;
  bool enq_seen;
  uint32_t enq_ms;  // start -> the ENQ read
  // Writes: the first byte of each segment and when it went out, from the
  // ENQ, or from the start without one.
  uint8_t segments;
  uint8_t sent_from[RAW_SCRIPT_WAITS_MAX + 1];
  uint32_t sent_at_ms[RAW_SCRIPT_WAITS_MAX + 1];
  // After the last write: the answer, and when its first byte came.
  uint8_t answer[RAW_SCRIPT_ANSWER_MAX];
  uint8_t answer_len;
  uint16_t answer_total;
  uint32_t answer_after_ms;
  // Bytes that arrived after the ENQ (or the start) and before the last
  // write, so not the answer; and bytes other than the ENQ while waiting for
  // it.
  uint8_t dropped[RAW_SCRIPT_DROPPED_MAX];
  uint8_t dropped_stored;
  uint16_t dropped_count;
  uint16_t noise_count;
};

template<class Port> class RawScript {
 public:
  explicit RawScript(Port *port) : port_(port) {}

  // Starts `spec`, a timed frame. Until loop() returns true the caller keeps
  // everything else off the port.
  void start(const RawSpec &spec, const RawScriptTiming &timing) {
    this->spec_ = spec;
    this->timing_ = timing;
    this->result_ = RawScriptResult{};
    this->next_ = 0;
    this->wait_idx_ = 0;
    this->p300_need_ = 0;
    this->start_ms_ = optolink_millis();
    this->origin_ms_ = this->start_ms_;
    this->mark_ms_ = this->start_ms_;
    const bool enq = spec.layer != RawLayer::P300 && !spec.now;
    this->state_ = enq ? State::WAIT_ENQ : State::SEND;
  }

  // One pass; true once the script has ended (result() holds it).
  bool loop() {
    if (this->state_ == State::IDLE)
      return false;
    if (this->state_ == State::DONE)
      return true;
    const uint32_t now = optolink_millis();
    if (now - this->start_ms_ > this->timing_.deadline_ms)
      return this->finish_(RawScriptStatus::TIMEOUT);
    switch (this->state_) {
      case State::WAIT_ENQ:
        return this->wait_enq_(now);
      case State::SEND:
        return this->send_();
      case State::RECEIVE:
        return this->receive_(now);
      default:
        return true;
    }
  }

  bool active() const { return this->state_ != State::IDLE && this->state_ != State::DONE; }
  const RawSpec &spec() const { return this->spec_; }
  const RawScriptResult &result() const { return this->result_; }
  // Ready for the next start().
  void clear() { this->state_ = State::IDLE; }

 private:
  enum class State : uint8_t { IDLE, WAIT_ENQ, SEND, RECEIVE, DONE };

  bool finish_(RawScriptStatus status) {
    this->result_.status = status;
    this->state_ = State::DONE;
    return true;
  }

  bool wait_enq_(uint32_t now) {
    while (this->port_->available()) {
      if (this->port_->read() == RAW_SCRIPT_ENQ) {
        // The frame goes out in this pass, as the engines answer an ENQ.
        this->origin_ms_ = optolink_millis();
        this->mark_ms_ = this->origin_ms_;
        this->result_.enq_seen = true;
        this->result_.enq_ms = this->origin_ms_ - this->start_ms_;
        this->state_ = State::SEND;
        return this->send_();
      }
      ++this->result_.noise_count;
    }
    if (now - this->start_ms_ > this->timing_.enq_timeout_ms)
      return this->finish_(RawScriptStatus::NO_ENQ);
    return false;
  }

  // Writes every segment that is due; a wait not yet over returns to the
  // next pass. Bytes arriving meanwhile are not the answer.
  bool send_() {
    this->drop_pending_();
    while (this->next_ < this->spec_.frame_len) {
      if (this->wait_idx_ < this->spec_.waits && this->spec_.wait_at[this->wait_idx_] == this->next_) {
        if (optolink_millis() - this->mark_ms_ < this->spec_.wait_ms[this->wait_idx_])
          return false;
        ++this->wait_idx_;
        this->drop_pending_();
      }
      const uint8_t end =
          this->wait_idx_ < this->spec_.waits ? this->spec_.wait_at[this->wait_idx_] : this->spec_.frame_len;
      const uint32_t t = optolink_millis();
      this->port_->write(&this->spec_.payload[this->next_], static_cast<uint8_t>(end - this->next_));
      RawScriptResult &r = this->result_;
      if (r.segments <= RAW_SCRIPT_WAITS_MAX) {
        r.sent_from[r.segments] = this->next_;
        r.sent_at_ms[r.segments] = t - this->origin_ms_;
        ++r.segments;
      }
      this->next_ = end;
      this->mark_ms_ = t;
    }
    this->rx_ms_ = this->mark_ms_;
    this->state_ = State::RECEIVE;
    return false;
  }

  void drop_pending_() {
    RawScriptResult &r = this->result_;
    while (this->port_->available()) {
      const uint8_t b = this->port_->read();
      if (r.dropped_stored < RAW_SCRIPT_DROPPED_MAX)
        r.dropped[r.dropped_stored++] = b;
      if (r.dropped_count < 0xFFFF)
        ++r.dropped_count;
    }
  }

  // The silence check comes before the drain, as in the engines: a byte read
  // after the deadline expired is not taken for the answer.
  bool receive_(uint32_t now) {
    RawScriptResult &r = this->result_;
    if (now - this->rx_ms_ > this->timing_.quiet_ms) {
      const bool collected = !this->spec_.answer_p300 && this->spec_.length == 0 && r.answer_total != 0;
      return this->finish_(collected ? RawScriptStatus::OK : RawScriptStatus::TIMEOUT);
    }
    while (this->port_->available()) {
      const uint8_t b = this->port_->read();
      const uint32_t t = optolink_millis();
      if (r.answer_total == 0)
        r.answer_after_ms = t - this->mark_ms_;
      if (r.answer_len < RAW_SCRIPT_ANSWER_MAX)
        r.answer[r.answer_len++] = b;
      if (r.answer_total < 0xFFFF)
        ++r.answer_total;
      this->rx_ms_ = now;
      if (this->spec_.answer_p300) {
        if (this->p300_byte_(b))
          return true;
      } else if (this->spec_.length != 0 && r.answer_total == this->spec_.length) {
        return this->finish_(RawScriptStatus::OK);
      }
    }
    return false;
  }

  // P300 answer: the ACK (06) or NACK (15), then 41 LL <LL bytes> CS. True
  // once it has ended.
  bool p300_byte_(uint8_t b) {
    const RawScriptResult &r = this->result_;
    if (this->p300_need_ == 0) {
      // Before the frame starts: an ACK, a NACK, or the 41 itself.
      if (r.answer_total == 1 && b == ACK)
        return false;
      if (r.answer_total == 1 && b == NACK)
        return this->finish_(RawScriptStatus::NACK);
      if (b != PACKET_START)
        return this->finish_(RawScriptStatus::MALFORMED);
      this->p300_start_ = static_cast<uint8_t>(r.answer_total - 1);
      this->p300_need_ = 0xFFFF;  // the length byte comes next
      return false;
    }
    const uint16_t in_frame = static_cast<uint16_t>(r.answer_total - this->p300_start_);
    if (this->p300_need_ == 0xFFFF) {
      // 41 LL <LL bytes> CS
      const uint16_t total = static_cast<uint16_t>(b) + 3;
      if (this->p300_start_ + total > RAW_SCRIPT_ANSWER_MAX)
        return this->finish_(RawScriptStatus::MALFORMED);
      this->p300_need_ = total;
      return false;
    }
    if (in_frame < this->p300_need_)
      return false;
    const uint8_t *frame = &r.answer[this->p300_start_];
    const uint8_t ll = frame[1];
    const bool sum_ok = p300_sum8(frame + 1, static_cast<std::size_t>(ll) + 1) == frame[ll + 2];
    return this->finish_(sum_ok ? RawScriptStatus::OK : RawScriptStatus::CHECKSUM);
  }

  static constexpr uint8_t ACK = 0x06;
  static constexpr uint8_t NACK = 0x15;
  static constexpr uint8_t PACKET_START = 0x41;

  Port *port_;
  State state_{State::IDLE};
  RawSpec spec_{};
  RawScriptTiming timing_{};
  RawScriptResult result_{};
  uint8_t next_{0};
  uint8_t wait_idx_{0};
  uint32_t start_ms_{0};
  uint32_t origin_ms_{0};
  uint32_t mark_ms_{0};
  uint32_t rx_ms_{0};
  uint8_t p300_start_{0};
  uint16_t p300_need_{0};
};

// Appends to `out` at `n`; a full buffer stops further output.
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
inline void raw_appendf(char *out, std::size_t cap, std::size_t &n, const char *fmt, ...) {
  if (n >= cap)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int k = std::vsnprintf(out + n, cap - n, fmt, ap);
  va_end(ap);
  n = k < 0 ? cap : std::min(cap, n + static_cast<std::size_t>(k));
}

inline void raw_append_hex(char *out, std::size_t cap, std::size_t &n, const uint8_t *data, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i)
    raw_appendf(out, cap, n, " %02X", data[i]);
}

// The spec as typed, canonically: "<layer> f [@now] <bytes, ~ms> [/ n | / p300]".
inline void format_raw_frame_spec(const RawSpec &spec, char *out, std::size_t cap, std::size_t &n) {
  raw_appendf(out, cap, n, "%s f%s", raw_layer_name(spec.layer), spec.now ? " @now" : "");
  uint8_t w = 0;
  for (uint8_t i = 0; i < spec.frame_len; ++i) {
    if (w < spec.waits && spec.wait_at[w] == i)
      raw_appendf(out, cap, n, " ~%u", static_cast<unsigned>(spec.wait_ms[w++]));
    raw_appendf(out, cap, n, " %02X", spec.payload[i]);
  }
  if (spec.answer_p300) {
    raw_appendf(out, cap, n, " / p300");
  } else if (spec.length != 0) {
    raw_appendf(out, cap, n, " / %u", static_cast<unsigned>(spec.length));
  }
}

// One line for the log and the scan result: the spec, then what happened.
inline void format_raw_script(const RawSpec &spec, const RawScriptResult &r, const RawScriptTiming &timing, char *out,
                              std::size_t cap) {
  if (cap == 0)
    return;
  out[0] = '\0';
  std::size_t n = 0;
  format_raw_frame_spec(spec, out, cap, n);
  raw_appendf(out, cap, n, ":");
  if (r.enq_seen)
    raw_appendf(out, cap, n, " ENQ after %u ms,", static_cast<unsigned>(r.enq_ms));
  if (r.status == RawScriptStatus::NO_ENQ) {
    raw_appendf(out, cap, n, " no ENQ within %u ms", static_cast<unsigned>(timing.enq_timeout_ms));
  } else {
    raw_appendf(out, cap, n, " sent");
    for (uint8_t s = 0; s < r.segments; ++s) {
      const uint8_t end = s + 1 < r.segments ? r.sent_from[s + 1] : spec.frame_len;
      raw_append_hex(out, cap, n, &spec.payload[r.sent_from[s]], static_cast<std::size_t>(end - r.sent_from[s]));
      raw_appendf(out, cap, n, " +%u ms%s", static_cast<unsigned>(r.sent_at_ms[s]), s + 1 < r.segments ? "," : ";");
    }
    // KW, GWG: a lone 05 on an unfinished answer is the device's next ENQ.
    const bool enq_only = spec.layer != RawLayer::P300 && r.status != RawScriptStatus::OK && r.answer_total == 1 &&
                          r.answer[0] == RAW_SCRIPT_ENQ;
    if (r.answer_total == 0) {
      raw_appendf(out, cap, n, " no answer");
    } else if (enq_only) {
      raw_appendf(out, cap, n, " no answer, ENQ +%u ms", static_cast<unsigned>(r.answer_after_ms));
    } else {
      raw_appendf(out, cap, n, " answer +%u ms:", static_cast<unsigned>(r.answer_after_ms));
      raw_append_hex(out, cap, n, r.answer, r.answer_len);
      if (r.answer_total > r.answer_len)
        raw_appendf(out, cap, n, " ... (%u bytes)", static_cast<unsigned>(r.answer_total));
    }
    if (r.status != RawScriptStatus::OK)
      raw_appendf(out, cap, n, " -- %s", raw_script_status_name(r.status));
  }
  if (r.dropped_count != 0) {
    raw_appendf(out, cap, n, "; %u other byte(s) before the last write:", static_cast<unsigned>(r.dropped_count));
    raw_append_hex(out, cap, n, r.dropped, r.dropped_stored);
  }
  if (r.noise_count != 0)
    raw_appendf(out, cap, n, "; %u byte(s) before the ENQ", static_cast<unsigned>(r.noise_count));
}

}  // namespace esphome::vitohome
