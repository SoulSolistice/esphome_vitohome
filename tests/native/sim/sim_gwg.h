#pragma once

// A GWG controller on the Optolink (openv wiki, Protokoll-GWG; docs/
// gwg_protocol.md; the #35 logs).
//
//   an ENQ every enq_min_ms .. enq_max_ms, the gap drawn from `seed`
//   on the ENQ, within quirks.enq_window_ms: 01 <telegram>
//   read       TT AD LEN 04            -> LEN bytes of TT's memory
//              AE (EEPROM)             -> per cell ~V V, or FF FF unserved
//              9E (BE), 6E (PORT)      -> one byte per cell
//   write      TT AD LEN <data> 04     -> 00, memory written
//              (TT AD LEN 04 <data> with write_eot_first)
//   EEPROM     AD AD ~V V 04           -> 00, V written if the pair sums to FF
//   burst      within burst_window_ms of an answer the next telegram may
//              follow without an ENQ, its 01 optional
//   unknown types are not answered

#include <array>
#include <cstdint>

#include "sim_port.h"

namespace sim {

// The defaults are #35's units', from their frame logs (2026-11-17 and -18).
struct GwgConfig {
  Quirks quirks = gwg_quirks();
  uint32_t enq_first_ms = 300;
  // ENQ to ENQ: 723 to 1731, median 1.2 s, after an answer as well.
  uint32_t enq_min_ms = 720;
  uint32_t enq_max_ms = 1730;
  uint32_t seed = 1;
  uint32_t burst_window_ms = 500;
  uint32_t byte_timeout_ms = 100;
  bool write_eot_first = false;
  // EEPROM cells that answer FF FF.
  std::array<bool, 256> eeprom_unserved{};

  static Quirks gwg_quirks() {
    Quirks q;
    // An EEPROM read -> its answer: 7 to 109, median 46 to 49.
    q.answer_delay_ms = 46;
    q.enq_window_ms = 100;
    return q;
  }
};

class SimGwg : public SimPort {
 public:
  // The memory of each access mode, in GWGAccessMode order: PHYSICAL,
  // VIRTUAL, EEPROM, XRAM, PORT, BE, KMBUS_RAM, KMBUS_EEPROM.
  std::array<std::array<uint8_t, 256>, 8> memory{};

  explicit SimGwg(const GwgConfig &config = {}) : SimPort(config.quirks), c_(config), rng_(config.seed) {
    for (std::size_t m = 0; m < memory.size(); ++m) {
      for (std::size_t a = 0; a < 256; ++a)
        memory[m][a] = static_cast<uint8_t>(SimMemory::pattern(static_cast<uint16_t>(a)) + m * 0x20);
    }
    next_enq_at_ = epoch_ + c_.enq_first_ms;
  }

  // What the device is; a replay adjusts it from the log.
  GwgConfig &config() { return c_; }
  uint32_t written_cells() const { return written_cells_; }

 protected:
  void on_time(uint32_t now) override {
    if (state_ == State::ENQ_OPEN && now - enq_at_ > q_.enq_window_ms)
      state_ = State::IDLE;
    if (state_ == State::BURST && past(now, answer_end_, c_.burst_window_ms))
      state_ = State::IDLE;
    if (state_ != State::IDLE || static_cast<int32_t>(now - next_enq_at_) < 0)
      return;
    // ENQs missed while the engine did not poll are not sent late.
    while (now - next_enq_at_ > c_.enq_max_ms)
      next_enq_at_ += this->gap();
    const uint32_t at = this->alive_from(next_enq_at_);
    if (at != next_enq_at_) {
      next_enq_at_ = at;
      if (static_cast<int32_t>(now - at) < 0)
        return;
    }
    this->send_enq(at);
    enq_at_ = at;
    state_ = now - at > q_.enq_window_ms ? State::IDLE : State::ENQ_OPEN;
    next_enq_at_ = at + this->gap();
  }

  void on_byte(uint8_t b, uint32_t now) override {
    if (state_ == State::TELEGRAM && now - last_rx_ms_ > c_.byte_timeout_ms)
      state_ = State::IDLE;
    if (state_ == State::ENQ_OPEN && now - enq_at_ > q_.enq_window_ms)
      state_ = State::IDLE;
    if (state_ == State::BURST && past(now, answer_end_, c_.burst_window_ms))
      state_ = State::IDLE;
    last_rx_ms_ = now;
    switch (state_) {
      case State::IDLE:
        return;
      case State::ENQ_OPEN:
        if (b == ENQ_ACK) {
          state_ = State::TELEGRAM;
          telegram_.clear();
        }
        return;
      case State::BURST:
        if (q_.refuse_chain)
          return;
        state_ = State::TELEGRAM;
        telegram_.clear();
        if (b == ENQ_ACK)
          return;
        [[fallthrough]];
      case State::TELEGRAM:
        telegram_.push_back(b);
        this->telegram_byte(now);
        return;
    }
  }

 private:
  enum class State : uint8_t { IDLE, ENQ_OPEN, TELEGRAM, BURST };

  static constexpr uint8_t EEPROM_READ = 0xAE;
  static constexpr uint8_t EEPROM_WRITE = 0xAD;

  // The access mode of a type byte, or -1.
  static int read_mode(uint8_t type) {
    static constexpr uint8_t kRead[8] = {0xCB, 0xC7, 0xAE, 0xC5, 0x6E, 0x9E, 0x33, 0x43};
    for (int m = 0; m < 8; ++m) {
      if (kRead[m] == type)
        return m;
    }
    return -1;
  }
  static int write_mode(uint8_t type) {
    static constexpr uint8_t kWrite[6] = {0xC8, 0xC4, 0xAD, 0xC3, 0x6D, 0x9D};
    static constexpr int kMode[6] = {0, 1, 2, 3, 4, 5};
    for (int i = 0; i < 6; ++i) {
      if (kWrite[i] == type)
        return kMode[i];
    }
    return -1;
  }

  uint32_t gap() {
    rng_ = rng_ * 1103515245u + 12345u;
    const uint32_t span = c_.enq_max_ms > c_.enq_min_ms ? c_.enq_max_ms - c_.enq_min_ms : 0;
    return c_.enq_min_ms + (span != 0 ? (rng_ >> 8) % (span + 1) : 0);
  }

  void telegram_byte(uint32_t now) {
    const uint8_t type = telegram_[0];
    const int rmode = read_mode(type);
    const int wmode = write_mode(type);
    if (rmode < 0 && wmode < 0) {
      state_ = State::IDLE;  // an unknown type: no answer
      return;
    }
    std::array<uint8_t, 2 * 64> answer{};
    std::size_t n = 0;
    const bool complete = type == EEPROM_WRITE ? telegram_.size() >= 5
                          : rmode >= 0         ? telegram_.size() >= 4
                                               : telegram_.size() >= 3 && telegram_.size() >= 4u + telegram_[2];
    if (complete && this->ignore_this()) {
      state_ = State::IDLE;
      return;
    }
    if (type == EEPROM_WRITE) {
      if (telegram_.size() < 5)
        return;
      if (telegram_[4] != EOT) {
        state_ = State::IDLE;
        return;
      }
      if (static_cast<uint8_t>(telegram_[2] + telegram_[3]) == 0xFF) {
        memory[2][telegram_[1]] = telegram_[3];
        ++written_cells_;
      }
      answer[n++] = 0x00;
    } else if (rmode >= 0) {
      if (telegram_.size() < 4)
        return;
      if (telegram_[3] != EOT) {
        state_ = State::IDLE;
        return;
      }
      const uint8_t address = telegram_[1];
      const uint8_t length = telegram_[2];
      for (uint8_t i = 0; i < length && n + 2 <= answer.size(); ++i) {
        const uint8_t a = static_cast<uint8_t>(address + i);
        if (type == EEPROM_READ) {
          const bool unserved = c_.eeprom_unserved[a];
          answer[n++] = unserved ? 0xFF : static_cast<uint8_t>(~memory[2][a]);
          answer[n++] = unserved ? 0xFF : memory[2][a];
        } else {
          answer[n++] = memory[rmode][a];
        }
      }
    } else {
      const uint8_t length = telegram_.size() >= 3 ? telegram_[2] : 0;
      if (telegram_.size() < 4u + length)
        return;
      const std::size_t data = c_.write_eot_first ? 4 : 3;
      const std::size_t eot = c_.write_eot_first ? 3 : 3u + length;
      if (telegram_[eot] != EOT) {
        state_ = State::IDLE;
        return;
      }
      for (uint8_t i = 0; i < length; ++i)
        memory[wmode][static_cast<uint8_t>(telegram_[1] + i)] = telegram_[data + i];
      written_cells_ += length;
      answer[n++] = 0x00;
    }
    answer_end_ = this->send_answer(answer.data(), n, now);
    state_ = State::BURST;
    next_enq_at_ = answer_end_ + this->gap();
  }

  GwgConfig c_;
  State state_ = State::IDLE;
  uint32_t rng_;
  uint32_t next_enq_at_ = 0;
  uint32_t enq_at_ = 0;
  uint32_t answer_end_ = 0;
  uint32_t last_rx_ms_ = 0;
  uint32_t written_cells_ = 0;
  std::vector<uint8_t> telegram_;
};

}  // namespace sim
