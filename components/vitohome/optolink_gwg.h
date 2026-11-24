// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_GWG_H
#define VITOHOME_OPTOLINK_GWG_H

// The GWG stack (design-optolink-layers.md, sections 4.3 and 5.1;
// docs/gwg_protocol.md): telegrams on an ENQ-paced line, the burst window
// between them, per-address reads; one request in flight, its watchdog and one
// result callback, as the KW exchange has them (optolink_exchange.h).
//
//   WAIT_ENQ  one byte per pass. An ENQ with a request waiting: the 01 and the
//             telegram in this pass, or HOLD. Any other byte goes to the
//             late-answer report. An empty line inside the burst window: the
//             telegram without its 01, in this pass.
//   HOLD      the line drained; ENQ_TO_SEND after the ENQ -> SEND
//   ACK_GAP   the 01 out, the line drained; ACK_TO_TELEGRAM after it -> SEND
//   SEND      the telegram; a partial write resumes on the next pass
//   RECEIVE   the answer deadline first, from the last byte written or read;
//             then the line drained into the answer. Exactly the bytes
//             expected -> the answer; an open frame that collects ends on the
//             deadline.
//   watchdog  3 s busy, from the submit, a retry or each cell of a run
//
// Burst window: 80 ms from the last answer, or from the ENQ a telegram went
// out on. Issue #35 (GWG_VBEM 0x2053 SW 0x1E, 40 h, 2026-11-19/20): sent
// under 80 ms after an answer, 109,520 answered and 1 not; at 80-99 ms, 183
// and 14; at 100-139 ms, 7 and 29. An error closes the window, a restart too;
// an open frame neither opens nor rides it.
//
// Burst retry and fallback: a burst-sent read met by the deadline goes out
// once more on the next ENQ. Answered there, the burst failed; after 2 such
// failures in a row, or 2 burst-sent writes unanswered, bursting stays off
// until a restart of the firmware. A burst-sent answer clears the count.
//
// Per-address reads: EEPROM, BE and PORT answer one cell per telegram
// (gwgCellLength()), so a run of N addresses is N telegrams, each LEN 1, each
// with its own watchdog. An EEPROM cell is the pair ~V V; FF FF is an address
// not in use, delivered as FF with Result::unserved_eeprom; any other pair
// that does not sum to FF fails the read with CRC.
//
// Late-answer report: after a deadline, the bytes of the telegram and any
// byte that follows within 2.5 s, until the next telegram goes out.
//
// Settable (design-optolink-layers.md, section 13): the timing points
// ENQ_TO_SEND and ACK_TO_TELEGRAM, 0 by default (the 01 and the telegram in
// one write, in the ENQ's pass), and the answer deadline (LinkWindow), 250 ms
// by default. Two variant switches change what is sent: the EOT before a
// write's payload, and the 01 on a burst-sent telegram.
//
// The request stays busy during its result callback, so a submit from inside
// it is refused.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "optolink_clock.h"
#include "optolink_codec_gwg.h"
#include "optolink_types.h"

namespace esphome::vitohome::optolink {

inline constexpr uint32_t GWG_WATCHDOG_MS = 3000;
// 250 ms: 2.7 times the slowest answer and a third of the shortest gap
// between two idle ENQs (749 ms) seen on a GWG unit (2026-08-23). Without
// it, the device's next ENQ completed a read it ignored, as the value 05.
inline constexpr uint32_t GWG_ANSWER_TIMEOUT_MS = 250;
inline constexpr uint32_t GWG_BURST_WINDOW_MS = 80;
inline constexpr uint8_t GWG_BURST_FAILURES = 2;
// The answer deadline's bounds. The upper one keeps a retried burst read, two
// deadlines and an ENQ wait, inside the hub's watchdog.
inline constexpr uint32_t GWG_ANSWER_TIMEOUT_MIN_MS = 50;
inline constexpr uint32_t GWG_ANSWER_TIMEOUT_MAX_MS = 2000;
// Longer than the longest answer deadline, so a device that needs that long
// shows as late, not silent.
inline constexpr uint32_t GWG_LATE_WATCH_MS = 2500;
static_assert(GWG_LATE_WATCH_MS > GWG_ANSWER_TIMEOUT_MAX_MS, "the late watch must outlast the longest deadline");
// The variant switches' defaults: the EOT after a write's payload, no 01 on a
// burst-sent telegram.
inline constexpr bool GWG_DEFAULT_WRITE_EOT_BEFORE_PAYLOAD = false;
inline constexpr bool GWG_DEFAULT_BURST_OMITS_ENQ_ACK = true;

// What arrived for a telegram that ran into the answer deadline, and after.
// Reported when anything arrived, and for a retried burst read in any case. A
// late 05 is not counted: it cannot be told from the device's ENQ.
struct GwgLateAnswer {
  uint16_t address;
  uint8_t received;        // bytes before the deadline
  uint8_t expected;        // bytes the telegram waited for
  uint8_t late_count;      // bytes after it, at most 255
  uint8_t late_stored;     // how many of them are in `late`
  uint32_t first_late_ms;  // the telegram's last byte -> the first late byte
  uint32_t deadline_ms;
  uint32_t enq_age_ms;  // as Timing::enq_age_ms
  bool retried;         // a burst-sent read that goes out again
  uint8_t late[8];
};

template<class Port> class GwgExchange {
 public:
  using ResultCallback = std::function<void(const Result &)>;
  using FallbackCallback = std::function<void(uint8_t failures)>;
  using LateAnswerCallback = std::function<void(const GwgLateAnswer &)>;

  explicit GwgExchange(Port *port) : port_(port), now_(optolink_millis()), last_ms_(now_) {}
  GwgExchange(const GwgExchange &) = delete;
  GwgExchange &operator=(const GwgExchange &) = delete;

  void on_result(ResultCallback callback) { on_result_ = std::move(callback); }
  // Once, when bursting goes off.
  void on_burst_fallback(FallbackCallback callback) { on_fallback_ = std::move(callback); }
  void on_late_answer(LateAnswerCallback callback) { on_late_ = std::move(callback); }
  // The trace for the recorder (LinkSink); none by default.
  void set_sink(const LinkSink &sink) { sink_ = sink; }

  static const char *state_name(uint8_t state) {
    static constexpr const char *NAMES[] = {"UNDEFINED", "WAIT_ENQ", "HOLD", "ACK_GAP", "SEND", "RECEIVE"};
    static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == static_cast<std::size_t>(State::RECEIVE) + 1, "a name per state");
    return state < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[state] : "?";
  }

  // A restart takes the clock afresh and knows nothing of the device's ENQs:
  // no burst before the next one, and an open late-answer report is closed.
  bool begin() {
    now_ = optolink_millis();
    sync_valid_ = false;
    this->close_late_watch_();
    this->set_state_(State::WAIT_ENQ);
    return port_->begin();
  }
  void end() {
    port_->end();
    this->set_state_(State::UNDEFINED);
    busy_ = false;
  }
  bool busy() const { return busy_; }
  // While a write waits on ENQ_TO_SEND or ACK_TO_TELEGRAM.
  bool wants_fast_loop() const { return state_ == State::HOLD || state_ == State::ACK_GAP; }

  // ENQ_TO_SEND or ACK_TO_TELEGRAM, `ms` clamped to its bound, from the next
  // telegram; false for another point.
  bool set_timing(TimingPoint point, uint32_t &ms) {
    if (point != TimingPoint::ENQ_TO_SEND && point != TimingPoint::ACK_TO_TELEGRAM)
      return false;
    if (ms > timing_max(point))
      ms = timing_max(point);
    (point == TimingPoint::ENQ_TO_SEND ? enq_to_send_set_ms_ : ack_to_telegram_set_ms_) = ms;
    return true;
  }
  uint32_t timing(TimingPoint point) const {
    switch (point) {
      case TimingPoint::ENQ_TO_SEND:
        return enq_to_send_set_ms_;
      case TimingPoint::ACK_TO_TELEGRAM:
        return ack_to_telegram_set_ms_;
      default:
        return 0;
    }
  }
  uint32_t timing_default(TimingPoint) const { return 0; }

  // The answer deadline, `ms` clamped to its bounds, from the next telegram.
  bool set_window(LinkWindow window, uint32_t &ms) {
    if (window != LinkWindow::ANSWER_DEADLINE)
      return false;
    if (ms < GWG_ANSWER_TIMEOUT_MIN_MS)
      ms = GWG_ANSWER_TIMEOUT_MIN_MS;
    if (ms > GWG_ANSWER_TIMEOUT_MAX_MS)
      ms = GWG_ANSWER_TIMEOUT_MAX_MS;
    answer_timeout_ms_ = ms;
    return true;
  }
  uint32_t window(LinkWindow window) const { return window == LinkWindow::ANSWER_DEADLINE ? answer_timeout_ms_ : 0; }
  uint32_t window_default(LinkWindow window) const {
    return window == LinkWindow::ANSWER_DEADLINE ? GWG_ANSWER_TIMEOUT_MS : 0;
  }

  // The variant switches, from the next telegram.
  void set_write_eot_before_payload(bool eot_first) { eot_first_ = eot_first; }
  bool write_eot_before_payload() const { return eot_first_; }
  void set_burst_omits_enq_ack(bool omit) { burst_omits_ack_ = omit; }
  bool burst_omits_enq_ack() const { return burst_omits_ack_; }

  // A GWG telegram by its TYPE byte, or an open frame; false while busy or
  // when the request cannot be one. An access mode's read or write keeps its
  // mode's handling; a two-byte EEPROM write is the pair as given, unchecked;
  // any other TYPE byte is answered with `length` bytes, or one after a
  // payload.
  bool submit(const Request &request) {
    switch (request.lane) {
      case Lane::GWG:
        return this->telegram_(request.code, request.address, request.length,
                               request.has_payload ? request.payload : nullptr);
      case Lane::FRAME:
        return this->open_frame_(request.payload, request.length, request.answer_length);
      case Lane::P300:
      case Lane::KW:
      default:
        return false;
    }
  }

  void loop() {
    const uint32_t previous = now_;
    now_ = optolink_millis();
    loop_gap_ms_ = now_ - previous;
    switch (state_) {
      case State::WAIT_ENQ:
        this->wait_enq_();
        break;
      case State::HOLD:
        this->hold_();
        break;
      case State::ACK_GAP:
        this->ack_gap_();
        break;
      case State::SEND:
        this->send_();
        break;
      case State::RECEIVE:
        this->receive_();
        break;
      case State::UNDEFINED:
        break;
    }
    if (busy_ && now_ - request_ms_ > this->watchdog_ms_()) {
      sink_.fact(LinkFact::WATCHDOG);
      never_sent_ = state_ == State::WAIT_ENQ;
      this->set_state_(State::WAIT_ENQ);
      this->fail_(OptolinkResult::TIMEOUT);
    }
    if (late_watch_ && now_ - late_.sent_ms > GWG_LATE_WATCH_MS)
      this->close_late_watch_();
  }

 private:
  enum class State : uint8_t { UNDEFINED, WAIT_ENQ, HOLD, ACK_GAP, SEND, RECEIVE };

  static constexpr uint8_t ENQ = 0x05;
  static constexpr uint8_t ENQ_ACK = 0x01;

  void set_state_(State state) {
    if (state != state_)
      sink_.state(static_cast<uint8_t>(state));
    state_ = state;
  }

  void drain_() {
    uint16_t drained = 0;
    while (port_->available()) {
      port_->read();
      ++drained;
    }
    if (drained != 0)
      sink_.fact(LinkFact::DRAINED, drained);
  }

  // The longest answer, MAX_PAYLOAD bytes, plus one: an EEPROM run receives
  // its last pair at the index of its last value.
  static constexpr std::size_t ANSWER_MAX = MAX_PAYLOAD + 1;

  struct LateWatch {
    GwgLateAnswer report;
    uint32_t sent_ms;  // the telegram's last byte
  };

  bool telegram_(uint8_t type, uint16_t address, uint8_t length, const uint8_t *data) {
    if (busy_)
      return false;
    const bool pair = data != nullptr && length == 2 && type == gwgWriteTypeByte(GWGAccessMode::EEPROM);
    for (uint8_t m = 0; !pair && m <= static_cast<uint8_t>(GWGAccessMode::KMBUS_EEPROM); ++m) {
      const auto mode = static_cast<GWGAccessMode>(m);
      if (data == nullptr && type == gwgReadTypeByte(mode))
        return this->read_(address, length, mode);
      if (data != nullptr && gwgModeIsWritable(mode) && type == gwgWriteTypeByte(mode))
        return this->write_(address, data, length, mode);
    }
    // A read's answer must fit; a payload is answered with one byte.
    if (data == nullptr && length > MAX_PAYLOAD)
      return false;
    Telegram telegram;
    if (!this->set_wire_(pair ? gwg_eeprom_pair(address, data[0], data[1], telegram)
                              : gwg_encode(type, address, length, data, eot_first_, telegram),
                         telegram))
      return false;
    this->start_(address, length, false);
    return true;
  }

  bool read_(uint16_t address, uint8_t length, GWGAccessMode mode) {
    if (length > MAX_PAYLOAD)
      return false;
    // A per-address run stays inside the 8-bit address space.
    const uint8_t cell = gwgCellLength(mode);
    if (cell != 0 && (length == 0 || address + length - 1 > 0xFF))
      return false;
    Telegram telegram;
    if (!this->set_wire_(
            gwg_encode_mode(gwgReadTypeByte(mode), address, cell != 0 ? 1 : length, nullptr, eot_first_, telegram),
            telegram))
      return false;
    this->start_(address, cell != 0 ? cell : length, false);
    per_address_ = cell != 0;
    run_mode_ = mode;
    run_length_ = cell != 0 ? length : 0;
    run_index_ = 0;
    return true;
  }

  // The KM-bus modes have no write TYPE byte.
  bool write_(uint16_t address, const uint8_t *data, uint8_t length, GWGAccessMode mode) {
    if (!gwgModeIsWritable(mode))
      return false;
    Telegram telegram;
    if (!this->set_wire_(gwg_encode_mode(gwgWriteTypeByte(mode), address, length, data, eot_first_, telegram),
                         telegram))
      return false;
    this->start_(address, length, false);
    return true;
  }

  // On the device's next ENQ, never in a burst; answered with `answer_length`
  // bytes, or with 0 whatever arrives until the deadline. Delivered at
  // address 0.
  bool open_frame_(const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
    if (busy_ || answer_length > MAX_PAYLOAD)
      return false;
    if (!frame_encode(bytes, length, GWG_FRAME_MAX, wire_))
      return false;
    this->start_(0, answer_length, true);
    return true;
  }

  // The wire bytes: the 01 that answers an ENQ, then the telegram.
  bool set_wire_(bool built, const Telegram &telegram) {
    if (!built) {
      wire_.length = 0;
      wire_.has_payload = false;
      return false;
    }
    wire_.bytes[0] = ENQ_ACK;
    std::memcpy(&wire_.bytes[1], telegram.bytes, telegram.length);
    wire_.length = static_cast<uint8_t>(telegram.length + 1);
    wire_.has_payload = telegram.has_payload;
    return true;
  }

  void start_(uint16_t address, uint8_t expect, bool frame) {
    address_ = address;
    expect_ = expect;
    per_address_ = false;
    unserved_ = false;
    never_sent_ = false;
    retrying_ = false;
    frame_ = frame;
    busy_ = true;
    // From the last pass's clock, as the watchdog compares against it.
    request_ms_ = now_;
  }

  // The deadline raised past its default lengthens the watchdog by as much,
  // so the ENQ wait it also covers is not eaten.
  uint32_t watchdog_ms_() const {
    return answer_timeout_ms_ > GWG_ANSWER_TIMEOUT_MS ? GWG_WATCHDOG_MS + (answer_timeout_ms_ - GWG_ANSWER_TIMEOUT_MS)
                                                      : GWG_WATCHDOG_MS;
  }

  bool can_burst_() const { return !burst_off_ && !frame_ && sync_valid_ && now_ - sync_ms_ <= GWG_BURST_WINDOW_MS; }

  // An open frame whose answer ends on a quiet line.
  bool collecting_() const { return frame_ && expect_ == 0; }

  void wait_enq_() {
    if (port_->available()) {
      const uint8_t byte = port_->read();
      if (byte == ENQ && busy_) {
        this->close_late_watch_();
        count_ = 0;
        enq_age_ms_ = loop_gap_ms_;
        sync_ms_ = now_;
        sync_valid_ = true;
        burst_sent_ = false;
        enq_seen_ms_ = optolink_millis();
        enq_paced_ = true;
        stamp_enq_to_send_ = true;
        ack_gap_ms_ = 0;
        if (enq_to_send_set_ms_ > 0) {
          this->set_state_(State::HOLD);
          this->hold_();
          return;
        }
        this->set_state_(State::SEND);
        this->send_();
        return;
      }
      // Not a usable ENQ: stop here, so the burst below only ever runs on an
      // empty line. A straggler may be a late answer.
      if (byte != ENQ)
        this->note_late_byte_(byte);
      return;
    }
    if (busy_ && this->can_burst_()) {
      this->close_late_watch_();
      count_ = burst_omits_ack_ ? 1 : 0;
      enq_age_ms_ = now_ - sync_ms_;
      burst_sent_ = true;
      enq_paced_ = false;
      stamp_enq_to_send_ = false;
      this->set_state_(State::SEND);
      this->send_();
    }
  }

  // A fresh clock: the delay is shorter than an idle loop pass. Nothing that
  // arrives meanwhile answers the telegram.
  void hold_() {
    this->drain_();
    if (optolink_millis() - enq_seen_ms_ < enq_to_send_set_ms_)
      return;
    this->set_state_(State::SEND);
    this->send_();
  }

  void ack_gap_() {
    this->drain_();
    const uint32_t elapsed = optolink_millis() - ack_sent_ms_;
    if (elapsed < ack_to_telegram_set_ms_)
      return;
    ack_gap_ms_ = elapsed;
    this->set_state_(State::SEND);
    this->send_();
  }

  void send_() {
    // With a gap set, a telegram on an ENQ goes out as its 01 alone first.
    const bool ack_alone =
        ack_to_telegram_set_ms_ > 0 && enq_paced_ && count_ == 0 && wire_.length > 1 && wire_.bytes[0] == ENQ_ACK;
    // Before the write, which may log.
    const uint32_t write_ms = optolink_millis();
    count_ = static_cast<uint8_t>(count_ + port_->write(&wire_.bytes[count_], ack_alone ? 1 : wire_.length - count_));
    if (stamp_enq_to_send_) {
      stamp_enq_to_send_ = false;
      enq_to_send_ms_ = write_ms - enq_seen_ms_;
    }
    if (ack_alone && count_ == 1) {
      ack_sent_ms_ = write_ms;
      this->set_state_(State::ACK_GAP);
      return;
    }
    if (count_ == wire_.length) {
      count_ = 0;
      last_ms_ = now_;
      send_done_ms_ = now_;
      this->set_state_(State::RECEIVE);
    }
  }

  void receive_() {
    // The deadline before the drain: bytes buffered past it, such as the
    // device's next ENQ held across a long pass, are stale, not the answer.
    if (now_ - last_ms_ > answer_timeout_ms_) {
      if (this->collecting_() && count_ != 0) {
        const uint8_t received = count_ < MAX_PAYLOAD ? count_ : MAX_PAYLOAD;
        count_ = 0;
        sync_valid_ = false;
        this->set_state_(State::WAIT_ENQ);
        this->deliver_(received, false);
        return;
      }
      const bool retry = burst_sent_ && !burst_off_ && !wire_.has_payload && !retrying_;
      // The buffer goes with the telegram, into the report: a stale ENQ left
      // there would send the next telegram on a window long closed.
      this->open_late_watch_(retry);
      while (port_->available())
        this->note_late_byte_(port_->read());
      count_ = 0;
      sync_valid_ = false;
      this->set_state_(State::WAIT_ENQ);
      if (retry) {
        sink_.fact(LinkFact::RETRY);
        retrying_ = true;
        request_ms_ = now_;
        return;
      }
      if (burst_sent_ && !burst_off_)
        this->note_burst_failure_();
      this->fail_(OptolinkResult::TIMEOUT);
      return;
    }
    // A cell of a run lands after the values collected so far.
    const std::size_t base = per_address_ ? run_index_ : 0;
    while (port_->available()) {
      const uint8_t byte = port_->read();
      if (base + count_ < ANSWER_MAX)
        answer_[base + count_] = byte;
      if (count_ < 0xFF)
        ++count_;
      last_ms_ = now_;
    }
    // Exactly the bytes expected: a longer answer runs into the deadline.
    const uint8_t expected = wire_.has_payload ? 1 : expect_;
    if (this->collecting_() || count_ != expected)
      return;
    count_ = 0;
    // An answer shows the device listening: the window opens again. Not after
    // an open frame, whose surplus bytes would complete the next telegram.
    sync_ms_ = now_;
    sync_valid_ = !frame_;
    if (burst_sent_) {
      burst_failures_ = 0;
    } else if (retrying_ && !burst_off_) {
      this->note_burst_failure_();
    }
    retrying_ = false;
    this->set_state_(State::WAIT_ENQ);
    if (per_address_) {
      if (run_mode_ == GWGAccessMode::EEPROM) {
        const uint8_t complement = answer_[run_index_];
        const uint8_t value = answer_[run_index_ + 1];
        const bool unserved = complement == 0xFF && value == 0xFF;
        if (!unserved && static_cast<uint8_t>(complement + value) != 0xFF) {
          this->fail_(OptolinkResult::CRC);
          return;
        }
        if (unserved)
          unserved_ = true;
        answer_[run_index_] = value;
      }
      ++run_index_;
      if (run_index_ < run_length_) {
        Telegram telegram;
        this->set_wire_(gwg_encode_mode(gwgReadTypeByte(run_mode_), static_cast<uint16_t>(address_ + run_index_), 1,
                                        nullptr, eot_first_, telegram),
                        telegram);
        request_ms_ = now_;
        return;
      }
    }
    this->deliver_(per_address_ ? run_length_ : expected, !wire_.has_payload);
  }

  void note_burst_failure_() {
    if (++burst_failures_ < GWG_BURST_FAILURES)
      return;
    burst_off_ = true;
    sink_.fact(LinkFact::BURST_OFF, burst_failures_);
    if (on_fallback_)
      on_fallback_(burst_failures_);
  }

  void open_late_watch_(bool retried) {
    this->close_late_watch_();
    late_watch_ = true;
    late_ = LateWatch{};
    late_.sent_ms = send_done_ms_;
    GwgLateAnswer &r = late_.report;
    r.address = address_;
    r.received = count_;
    r.expected = wire_.has_payload ? 1 : expect_;
    r.deadline_ms = answer_timeout_ms_;
    r.enq_age_ms = enq_age_ms_;
    r.retried = retried;
  }

  void note_late_byte_(uint8_t byte) {
    if (!late_watch_)
      return;
    GwgLateAnswer &r = late_.report;
    if (r.late_count == 0)
      r.first_late_ms = now_ - late_.sent_ms;
    if (r.late_stored < sizeof(r.late))
      r.late[r.late_stored++] = byte;
    if (r.late_count < 0xFF)
      ++r.late_count;
  }

  void close_late_watch_() {
    if (!late_watch_)
      return;
    late_watch_ = false;
    const GwgLateAnswer &r = late_.report;
    if (r.late_count != 0)
      sink_.fact(LinkFact::LATE, r.late_count);
    if ((r.received != 0 || r.late_count != 0 || r.retried) && on_late_)
      on_late_(r);
  }

  Result result_(OptolinkResult status) const {
    Result result{};
    result.status = status;
    result.address = address_;
    result.timing.enq_paced = enq_paced_;
    result.timing.enq_age_ms = enq_age_ms_;
    result.timing.enq_to_send_ms = enq_to_send_ms_;
    result.timing.ack_gap_ms = ack_gap_ms_;
    return result;
  }

  // `timed`: a read's answer, whose time from the telegram's last byte is
  // reported.
  void deliver_(uint8_t length, bool timed) {
    if (on_result_) {
      Result result = this->result_(OptolinkResult::OK);
      result.data = answer_;
      result.length = length;
      result.unserved_eeprom = unserved_;
      result.timing.answer_timed = timed;
      result.timing.send_to_answer_ms = timed ? now_ - send_done_ms_ : 0;
      on_result_(result);
    }
    busy_ = false;
    retrying_ = false;
  }

  void fail_(OptolinkResult status) {
    retrying_ = false;
    if (on_result_) {
      Result result = this->result_(status);
      result.never_sent = status == OptolinkResult::TIMEOUT && never_sent_;
      on_result_(result);
    }
    busy_ = false;
  }

  Port *port_;
  State state_{State::UNDEFINED};
  uint32_t now_;
  // The telegram's last byte, then the answer's latest.
  uint32_t last_ms_;
  uint32_t request_ms_{0};
  uint32_t loop_gap_ms_{0};

  // How the telegram went out (Timing).
  uint32_t enq_age_ms_{0};
  uint32_t send_done_ms_{0};
  uint32_t enq_seen_ms_{0};
  uint32_t enq_to_send_ms_{0};
  bool enq_paced_{false};
  bool stamp_enq_to_send_{false};
  uint32_t ack_sent_ms_{0};
  uint32_t ack_gap_ms_{0};

  // The burst window and its fallback.
  uint32_t sync_ms_{0};
  bool sync_valid_{false};
  bool burst_sent_{false};
  uint8_t burst_failures_{0};
  bool burst_off_{false};
  // The telegram in flight is a burst-sent read's retry on an ENQ.
  bool retrying_{false};

  bool late_watch_{false};
  LateWatch late_{};

  // Bytes written of the telegram while sending, of the answer while
  // receiving.
  uint8_t count_{0};
  uint16_t address_{0};
  // The answer's bytes: a cell's in a run.
  uint8_t expect_{0};
  // A run: the request's length, and the cell in flight.
  bool per_address_{false};
  GWGAccessMode run_mode_{GWGAccessMode::PHYSICAL};
  uint8_t run_length_{0};
  uint8_t run_index_{0};
  bool unserved_{false};
  bool never_sent_{false};
  bool frame_{false};
  bool busy_{false};

  bool eot_first_{GWG_DEFAULT_WRITE_EOT_BEFORE_PAYLOAD};
  bool burst_omits_ack_{GWG_DEFAULT_BURST_OMITS_ENQ_ACK};
  uint32_t answer_timeout_ms_{GWG_ANSWER_TIMEOUT_MS};
  uint32_t enq_to_send_set_ms_{0};
  uint32_t ack_to_telegram_set_ms_{0};

  Telegram wire_{};
  uint8_t answer_[ANSWER_MAX]{};
  ResultCallback on_result_;
  FallbackCallback on_fallback_;
  LateAnswerCallback on_late_;
  LinkSink sink_;
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_GWG_H
