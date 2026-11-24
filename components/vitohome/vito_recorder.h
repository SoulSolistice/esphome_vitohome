#pragma once

// The recorder (docs/design_notes.md, section 9b): the last bytes on the
// line, the link's states and facts (LinkSink) and the result of every
// request, with µs times, in a ring of fixed
// size. It freezes some entries after the first failure it is set to watch,
// keeps them until they are dumped, and dumps them a few lines per loop pass.
//
// Loop context only: the port records from the engine's read() and write(),
// the hub its results and passes. No lock, no allocation after allocate().
//
// The time is the reading of the clock passed in (micros() on a device). An
// RX byte is stamped when it is read from the UART buffer, not when it
// arrived, so `pass` says which bytes one loop pass handled. A write is
// stamped before it is handed to the UART.

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>

#include "optolink_types.h"

namespace esphome::vitohome {

enum class RecordKind : uint8_t {
  RX,
  TX,
  // value: the OptolinkResult, RECORDER_SCRIPT added for a timed frame's.
  RESULT,
  // A 16-bit argument of the entry before it, in `pass`; value: a RecordArg.
  ARG,
  // value: the RecorderTrigger that froze the ring.
  TRIGGER,
  // value: the link's state, named by the exchange's state_name().
  STATE,
  // value: a LinkFact; its argument, if any, in an ARG entry after it.
  FACT,
};

enum class RecordArg : uint8_t { ADDRESS, FACT };

// Which failures freeze the ring; none: only a dump does.
enum RecorderTrigger : uint8_t {
  RECORDER_FREEZE_TIMEOUT = 1U << 0,
  // Any result but OK, a timeout too.
  RECORDER_FREEZE_ERROR = 1U << 1,
  // A telegram met by silence that goes out again (LinkFact::RETRY), even
  // when the retry is answered.
  RECORDER_FREEZE_RETRY = 1U << 2,
};

struct RecorderEntry {
  uint32_t t_us;
  uint16_t pass;
  uint8_t kind;
  uint8_t value;
};
static_assert(sizeof(RecorderEntry) == 8, "8 bytes an entry");

class VitoRecorder {
 public:
  using Clock = uint32_t (*)();
  using StateName = const char *(*)(uint8_t);
  static constexpr uint8_t RECORDER_SCRIPT = 0x80;
  static constexpr uint16_t MIN_SIZE = 32;
  static constexpr uint16_t MAX_SIZE = 4096;
  // Bytes of one direction and pass on one dump line.
  static constexpr uint8_t BYTES_PER_LINE = 24;
  static constexpr std::size_t LINE_CAPACITY = 32 + 3 * BYTES_PER_LINE;

  // Once, in setup(). False: a setting is out of range or the allocation
  // failed; the recorder stays off.
  bool allocate(uint16_t size, uint16_t after, uint8_t freeze_on, Clock clock) {
    if (entries_ != nullptr || size < MIN_SIZE || size > MAX_SIZE || after >= size || clock == nullptr)
      return false;
    entries_.reset(new (std::nothrow) RecorderEntry[size]);
    if (entries_ == nullptr)
      return false;
    size_ = size;
    after_ = after;
    freeze_on_ = freeze_on;
    clock_ = clock;
    this->arm();
    return true;
  }

  bool enabled() const { return entries_ != nullptr; }
  uint16_t size() const { return size_; }
  uint16_t held() const { return count_; }
  bool frozen() const { return state_ == State::FROZEN || state_ == State::DUMPING; }
  bool dumping() const { return state_ == State::DUMPING; }
  // Frozen by a failure, its after-entries taken, not dumped yet.
  bool frozen_by_failure() const { return state_ == State::FROZEN && trigger_ != 0; }
  uint16_t triggers_since() const { return triggers_since_; }

  // The hub's loop pass, stamped on every entry from here on.
  void set_pass(uint16_t pass) { pass_ = pass; }
  uint32_t now() const { return clock_ != nullptr ? clock_() : 0; }

  void rx(uint8_t byte) { this->record_(this->now(), RecordKind::RX, byte); }
  // `t_us`: the clock read before the write was handed on.
  void tx(uint32_t t_us, const uint8_t *data, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i)
      this->record_(t_us, RecordKind::TX, data[i]);
  }

  // The names of the link's states in the dump.
  void set_state_names(StateName names) { state_names_ = names; }

  // A LinkSink's emit, `context` the recorder.
  static void link_trace(void *context, optolink::LinkTrace trace, uint8_t value, uint16_t arg) {
    auto *self = static_cast<VitoRecorder *>(context);
    const uint32_t t = self->now();
    if (trace == optolink::LinkTrace::STATE) {
      self->record_(t, RecordKind::STATE, value);
      return;
    }
    self->record_(t, RecordKind::FACT, value);
    if (arg != 0)
      self->record_arg_(t, RecordArg::FACT, arg);
    if (value == static_cast<uint8_t>(optolink::LinkFact::RETRY) && (self->freeze_on_ & RECORDER_FREEZE_RETRY) != 0)
      self->on_failure_(t, RECORDER_FREEZE_RETRY);
  }

  // A request's end; `script`: a timed frame's, mapped onto OptolinkResult.
  void result(optolink::OptolinkResult status, uint16_t address, bool script = false) {
    if (!this->enabled())
      return;
    const uint32_t t = this->now();
    const auto code = static_cast<uint8_t>(status);
    this->record_(t, RecordKind::RESULT, static_cast<uint8_t>(script ? code | RECORDER_SCRIPT : code));
    this->record_arg_(t, RecordArg::ADDRESS, address);
    if (status == optolink::OptolinkResult::TIMEOUT && (freeze_on_ & RECORDER_FREEZE_TIMEOUT) != 0) {
      this->on_failure_(t, RECORDER_FREEZE_TIMEOUT);
    } else if (status != optolink::OptolinkResult::OK && (freeze_on_ & RECORDER_FREEZE_ERROR) != 0) {
      this->on_failure_(t, RECORDER_FREEZE_ERROR);
    }
  }

  // Clears the ring and records again.
  void arm() {
    head_ = 0;
    count_ = 0;
    remaining_after_ = 0;
    triggers_since_ = 0;
    trigger_ = 0;
    state_ = State::RECORDING;
  }

  // Freezes the ring if it is not frozen and starts the dump. False while one
  // runs, or when off.
  bool start_dump() {
    if (!this->enabled() || state_ == State::DUMPING)
      return false;
    state_ = State::DUMPING;
    cursor_ = 0;
    header_done_ = false;
    return true;
  }

  // Formats the next dump line into `line`; false when the dump is complete,
  // which re-arms the ring.
  bool next_dump_line(char *line, std::size_t capacity) {
    if (state_ != State::DUMPING || capacity == 0)
      return false;
    if (!header_done_) {
      header_done_ = true;
      t0_ = count_ != 0 ? this->at_(0).t_us : 0;
      std::snprintf(line, capacity, "rec dump: %u of %u entries, frozen by %s, %u failure(s) since",
                    static_cast<unsigned>(count_), static_cast<unsigned>(size_),
                    trigger_ != 0 ? trigger_name(trigger_) : "the dump", static_cast<unsigned>(triggers_since_));
      return true;
    }
    if (cursor_ >= count_) {
      this->arm();
      return false;
    }
    this->format_line_(line, capacity);
    return true;
  }

  static const char *trigger_name(uint8_t trigger) {
    switch (trigger) {
      case RECORDER_FREEZE_TIMEOUT:
        return "a timeout";
      case RECORDER_FREEZE_ERROR:
        return "an error";
      case RECORDER_FREEZE_RETRY:
        return "a retry";
      default:
        return "nothing";
    }
  }

  static const char *status_name(uint8_t value) {
    const auto status = static_cast<optolink::OptolinkResult>(value & static_cast<uint8_t>(~RECORDER_SCRIPT));
    return status == optolink::OptolinkResult::OK ? "ok" : optolink::errorToString(status);
  }

 private:
  enum class State : uint8_t { RECORDING, AFTER, FROZEN, DUMPING };

  const RecorderEntry &at_(uint16_t i) const {
    const uint16_t oldest = count_ < size_ ? 0 : head_;
    return entries_[(oldest + i) % size_];
  }

  void record_(uint32_t t_us, RecordKind kind, uint8_t value) {
    if (entries_ == nullptr || state_ == State::FROZEN || state_ == State::DUMPING)
      return;
    entries_[head_] = RecorderEntry{t_us, pass_, static_cast<uint8_t>(kind), value};
    head_ = static_cast<uint16_t>((head_ + 1) % size_);
    if (count_ < size_)
      ++count_;
    if (state_ == State::AFTER && --remaining_after_ == 0)
      state_ = State::FROZEN;
  }

  void record_arg_(uint32_t t_us, RecordArg arg, uint16_t value) {
    const uint16_t pass = pass_;
    pass_ = value;
    this->record_(t_us, RecordKind::ARG, static_cast<uint8_t>(arg));
    pass_ = pass;
  }

  void on_failure_(uint32_t t_us, uint8_t trigger) {
    if (state_ != State::RECORDING) {
      if (triggers_since_ != UINT16_MAX)
        ++triggers_since_;
      return;
    }
    trigger_ = trigger;
    if (after_ == 0) {
      this->record_(t_us, RecordKind::TRIGGER, trigger);
      state_ = State::FROZEN;
      return;
    }
    remaining_after_ = after_;
    this->record_(t_us, RecordKind::TRIGGER, trigger);
    state_ = State::AFTER;
  }

  const char *state_name_(uint8_t state) const { return state_names_ != nullptr ? state_names_(state) : "?"; }

  static void append_(std::size_t capacity, std::size_t &pos, int written) {
    if (written > 0)
      pos += static_cast<std::size_t>(written);
    if (pos >= capacity)
      pos = capacity - 1;
  }

  // One line: `rec +<ms>.<µs> p<pass> <dir> <bytes>`, the bytes of one
  // direction read or written in one pass joined; a result as
  // `= <status> @<address>`, the trigger as `! <name>`, the link's states of
  // one pass as `~ <name> ...`, a fact as `* <name> [<arg>]`.
  void format_line_(char *line, std::size_t capacity) {
    const RecorderEntry &first = this->at_(cursor_);
    const uint32_t rel = first.t_us - t0_;
    std::size_t pos = 0;
    append_(capacity, pos,
            std::snprintf(line, capacity, "rec +%" PRIu32 ".%03" PRIu32 " p%u ", rel / 1000U, rel % 1000U,
                          static_cast<unsigned>(first.pass)));
    const auto kind = static_cast<RecordKind>(first.kind);
    ++cursor_;
    switch (kind) {
      case RecordKind::RESULT: {
        uint16_t address = 0;
        if (cursor_ < count_ && static_cast<RecordKind>(this->at_(cursor_).kind) == RecordKind::ARG) {
          address = this->at_(cursor_).pass;
          ++cursor_;
        }
        std::snprintf(line + pos, capacity - pos, "= %s%s @%04X", (first.value & RECORDER_SCRIPT) != 0 ? "script " : "",
                      status_name(first.value), static_cast<unsigned>(address));
        return;
      }
      case RecordKind::TRIGGER:
        std::snprintf(line + pos, capacity - pos, "! %s", trigger_name(first.value));
        return;
      case RecordKind::ARG:
        std::snprintf(line + pos, capacity - pos, "arg %u %u", static_cast<unsigned>(first.value),
                      static_cast<unsigned>(first.pass));
        return;
      case RecordKind::FACT: {
        const char *name = first.value < optolink::LINK_FACTS
                               ? optolink::link_fact_name(static_cast<optolink::LinkFact>(first.value))
                               : "?";
        if (cursor_ < count_ && static_cast<RecordKind>(this->at_(cursor_).kind) == RecordKind::ARG &&
            this->at_(cursor_).value == static_cast<uint8_t>(RecordArg::FACT)) {
          std::snprintf(line + pos, capacity - pos, "* %s %u", name, static_cast<unsigned>(this->at_(cursor_).pass));
          ++cursor_;
        } else {
          std::snprintf(line + pos, capacity - pos, "* %s", name);
        }
        return;
      }
      case RecordKind::STATE:
        // The states of one pass on one line.
        append_(capacity, pos, std::snprintf(line + pos, capacity - pos, "~ %s", this->state_name_(first.value)));
        while (cursor_ < count_ && pos + 16 < capacity) {
          const RecorderEntry &next = this->at_(cursor_);
          if (next.kind != first.kind || next.pass != first.pass)
            break;
          append_(capacity, pos, std::snprintf(line + pos, capacity - pos, " %s", this->state_name_(next.value)));
          ++cursor_;
        }
        return;
      case RecordKind::RX:
      case RecordKind::TX:
        break;
    }
    append_(capacity, pos,
            std::snprintf(line + pos, capacity - pos, "%s %02X", kind == RecordKind::TX ? ">>>" : "<<<",
                          static_cast<unsigned>(first.value)));
    uint8_t joined = 1;
    while (cursor_ < count_ && joined < BYTES_PER_LINE && pos + 4 < capacity) {
      const RecorderEntry &next = this->at_(cursor_);
      if (next.kind != first.kind || next.pass != first.pass)
        break;
      append_(capacity, pos, std::snprintf(line + pos, capacity - pos, ":%02X", static_cast<unsigned>(next.value)));
      ++cursor_;
      ++joined;
    }
  }

  std::unique_ptr<RecorderEntry[]> entries_;
  Clock clock_{nullptr};
  StateName state_names_{nullptr};
  uint16_t size_{0};
  uint16_t after_{0};
  uint16_t head_{0};
  uint16_t count_{0};
  uint16_t remaining_after_{0};
  uint16_t triggers_since_{0};
  uint16_t pass_{0};
  uint16_t cursor_{0};
  uint32_t t0_{0};
  uint8_t freeze_on_{0};
  uint8_t trigger_{0};
  bool header_done_{false};
  State state_{State::RECORDING};
};

}  // namespace esphome::vitohome
