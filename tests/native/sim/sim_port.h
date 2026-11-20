#pragma once

// Device simulators for the native tests: a controller behind the port that
// answers what the engine sends, on the host test clock (design section
// 16.5). Unlike FakeOptolink, which plays a script, a simulator reacts: it
// sends its ENQs, parses each telegram and answers from its memory, so a test
// states what the device is, not which bytes arrive when.
//
// SimPort is the port side every simulator shares: what the engine writes
// reaches on_byte() at once; what the device sends is scheduled with a due
// time and becomes readable once the clock reaches it. on_time() lets the
// device act on its own (an ENQ) whenever the engine polls. Everything runs
// on optolink_millis(), so a frozen clock makes a run deterministic, and two
// simulators built from the same Config behave the same, the premise of the
// differential target.
//
// Quirks are the field findings as parameters (16.5): each confirmed one
// becomes a switch here and a test in proof_sim.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <utility>
#include <vector>

#include "optolink/helpers.h"  // optolink_millis(): the host test clock

namespace sim {

struct Quirks {
  // Last byte of a telegram -> first byte of the answer.
  uint32_t answer_delay_ms = 25;
  // Between two bytes of an answer; 0 sends them together.
  uint32_t answer_byte_gap_ms = 0;
  // ENQ-paced links: the master's first byte must follow the ENQ within this.
  uint32_t enq_window_ms = 200;
  // A telegram not sent on an ENQ (chained, burst) is ignored.
  bool refuse_chain = false;
  // One telegram in this many goes unanswered, chosen by `seed` (0: none).
  // #35: a GWG unit ignored 16 to 20 % of its reads.
  uint32_t ignore_one_in = 0;
  uint32_t seed = 1;
  // Bytes added to (0xEE each) or cut from the end of every answer.
  uint8_t answer_extra = 0;
  uint8_t answer_missing = 0;
  // P300: the 06 before an answer frame is not sent.
  bool swallow_ack = false;
  // The device ignores the line and sends nothing for dead_ms of every
  // dead_period_ms (0: never).
  uint32_t dead_period_ms = 0;
  uint32_t dead_ms = 0;
};

class SimPort {
 public:
  explicit SimPort(const Quirks &quirks) : q_(quirks), epoch_(optolink_millis()), draw_(quirks.seed) {}
  virtual ~SimPort() = default;
  SimPort(const SimPort &) = delete;
  SimPort &operator=(const SimPort &) = delete;

  // The port the engine drives.
  bool begin() { return true; }
  void end() {}
  std::size_t write(const uint8_t *data, uint8_t length) {
    const uint32_t now = optolink_millis();
    written_.insert(written_.end(), data, data + length);
    for (uint8_t i = 0; i < length; ++i) {
      if (!this->dead(now))
        this->on_byte(data[i], now);
    }
    return length;
  }
  std::size_t available() {
    const uint32_t now = optolink_millis();
    this->on_time(now);
    std::size_t n = 0;
    for (const auto &entry : out_) {
      if (static_cast<int32_t>(now - entry.first) < 0)
        break;
      ++n;
    }
    return n;
  }
  uint8_t read() {
    if (this->available() == 0)
      return 0;
    const uint8_t b = out_.front().second;
    out_.pop_front();
    return b;
  }

  // For tests.
  const std::vector<uint8_t> &written() const { return written_; }
  // Whether the next complete telegram goes unanswered (a replayed log
  // shows it so).
  void ignore_next_telegram(bool ignore = true) { ignore_next_ = ignore; }

  uint32_t enq_count() const { return enq_count_; }
  uint32_t answered() const { return answered_; }
  const Quirks &quirks() const { return q_; }
  Quirks &quirks() { return q_; }

 protected:
  static constexpr uint8_t ENQ = 0x05;
  static constexpr uint8_t ENQ_ACK = 0x01;
  static constexpr uint8_t EOT = 0x04;
  static constexpr uint8_t ACK = 0x06;
  static constexpr uint8_t NACK = 0x15;

  virtual void on_byte(uint8_t b, uint32_t now) = 0;
  virtual void on_time(uint32_t now) = 0;

  // Whether more than `span` ms passed since `since`; false while `since`
  // is still ahead, as an answer's end is until it has gone out.
  static bool past(uint32_t now, uint32_t since, uint32_t span) {
    return static_cast<int32_t>(now - since) > static_cast<int32_t>(span);
  }

  bool dead(uint32_t now) const {
    return q_.dead_period_ms != 0 && q_.dead_ms != 0 && (now - epoch_) % q_.dead_period_ms < q_.dead_ms;
  }
  // The end of the dead phase `now` is in, or `now`.
  uint32_t alive_from(uint32_t now) const {
    return this->dead(now) ? now - (now - epoch_) % q_.dead_period_ms + q_.dead_ms : now;
  }

  // Schedules bytes from `at` on; returns when the last is due.
  uint32_t send(const uint8_t *data, std::size_t length, uint32_t at, uint32_t gap_ms = 0) {
    uint32_t due = at;
    if (!out_.empty() && static_cast<int32_t>(out_.back().first - due) > 0)
      due = out_.back().first;
    for (std::size_t i = 0; i < length; ++i) {
      if (i != 0)
        due += gap_ms;
      out_.emplace_back(due, data[i]);
    }
    return due;
  }
  void send_enq(uint32_t at) {
    this->send(&ENQ, 1, at);
    ++enq_count_;
  }
  // An answer with the extra/missing quirks applied, `answer_delay_ms` after
  // `now`, and the bytes from `split_at` on `split_gap_ms` after the rest;
  // returns when its last byte is due.
  uint32_t send_answer(const uint8_t *data, std::size_t length, uint32_t now, std::size_t split_at = 0,
                       uint32_t split_gap_ms = 0) {
    std::vector<uint8_t> bytes(data, data + length);
    bytes.resize(bytes.size() > q_.answer_missing ? bytes.size() - q_.answer_missing : 0);
    bytes.insert(bytes.end(), q_.answer_extra, 0xEE);
    ++answered_;
    if (bytes.empty())
      return now + q_.answer_delay_ms;
    if (split_at == 0 || split_at >= bytes.size())
      return this->send(bytes.data(), bytes.size(), now + q_.answer_delay_ms, q_.answer_byte_gap_ms);
    const uint32_t first = this->send(bytes.data(), split_at, now + q_.answer_delay_ms, q_.answer_byte_gap_ms);
    return this->send(bytes.data() + split_at, bytes.size() - split_at, first + split_gap_ms, q_.answer_byte_gap_ms);
  }
  bool output_pending() const { return !out_.empty(); }

  // Whether to leave a complete telegram unanswered: ignore_next_telegram()
  // or the ignore_one_in draw.
  bool ignore_this() {
    if (ignore_next_) {
      ignore_next_ = false;
      return true;
    }
    if (q_.ignore_one_in == 0)
      return false;
    draw_ = draw_ * 1103515245u + 12345u;
    return (draw_ >> 8) % q_.ignore_one_in == 0;
  }

  Quirks q_;
  // The clock when the device was built: the origin of its own timetable.
  uint32_t epoch_;

 private:
  std::deque<std::pair<uint32_t, uint8_t>> out_;
  std::vector<uint8_t> written_;
  uint32_t enq_count_ = 0;
  uint32_t answered_ = 0;
  bool ignore_next_ = false;
  uint32_t draw_;
};

// The device's datapoints: 64 KiB, each cell a pattern of its address unless
// a test sets it, so a read returns something a test can predict.
class SimMemory {
 public:
  SimMemory() {
    for (std::size_t a = 0; a < cells_.size(); ++a)
      cells_[a] = pattern(static_cast<uint16_t>(a));
  }
  static uint8_t pattern(uint16_t address) {
    return static_cast<uint8_t>((address * 7 + (address >> 8) * 3 + 0x11) & 0xFF);
  }
  uint8_t get(uint16_t address) const { return cells_[address]; }
  void set(uint16_t address, uint8_t value) { cells_[address] = value; }
  void set(uint16_t address, std::initializer_list<uint8_t> values) {
    for (uint8_t v : values)
      cells_[address++] = v;
  }
  void read(uint16_t address, uint8_t length, uint8_t *out) const {
    for (uint8_t i = 0; i < length; ++i)
      out[i] = cells_[static_cast<uint16_t>(address + i)];
  }

 private:
  std::array<uint8_t, 65536> cells_{};
};

}  // namespace sim
