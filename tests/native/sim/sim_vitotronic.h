#pragma once

// A Vitotronic on the Optolink: KW, and with `p300` set the P300 session it
// enters on 16 00 00. One device in two modes, as on a VScotHO1_72, so the
// P300 engine's KW detour drops to KW with an EOT and comes back the same way.
//
// KW (openv wiki, Protokoll KW; the VScotHO1_72 captures and timed frames):
//   idle          an ENQ every enq_period_ms
//   on the ENQ, within quirks.enq_window_ms:
//                 01 <telegram>   a KW telegram
//                 16 00 00        into P300 (p300 only), answered 06
//   telegram      F7 AH AL LEN               -> LEN bytes of memory
//                 F4 AH AL LEN <LEN bytes>   -> 00, memory written
//                 6B AH AL LEN               -> LEN bytes of the burner control
//                 any other type             -> no answer
//   after an answer the next telegram may follow without ENQ or 01 until the
//   next ENQ, post_answer_enq_ms after the answer
//   04            the next ENQ eot_kw_enq_ms later
//
// P300 (openv wiki, Protokoll 300; the VScotHO1_72 long run and timed frames):
//   41 LL 00 FC AH AL DL [payload] CS   -> 06, then the answer frame
//       FC 01 READ   DL bytes of memory; error 01 for an unserved address
//       FC 02 WRITE  payload written; answered without data
//       FC 07 RPC    answered with rpc_answer
//       any other    error 05 (so is a GFA read, 6B, as on the VScotHO1_72)
//   an error frame comes as 41 LL, then the rest p300_error_gap_ms later;
//   until the engine ACKs it, an EOT is answered with 06 and ends the session
//   a bad checksum, or a pause over p300_start_gap_ms after the 41 -> 15
//   16 00 00 -> 06 (keepalive); 04 -> KW, the next ENQ eot_session_enq_ms later

#include <cstdint>
#include <utility>
#include <vector>

#include "sim_port.h"

namespace sim {

// The defaults are a VScotHO1_72's, from its frame logs (2026-04, P300) and
// timed frames (2026-11-19, KW and P300), in ms.
struct VitotronicConfig {
  Quirks quirks = vitotronic_quirks();
  bool p300 = true;
  uint32_t enq_first_ms = 500;    // build -> first ENQ
  uint32_t enq_period_ms = 2200;  // idle ENQ to ENQ: 2218
  // An answer -> the next ENQ: 2968 to 2984.
  uint32_t post_answer_enq_ms = 2970;
  // An EOT in KW -> the next ENQ: 786 to 810 (n=16, one at 530).
  uint32_t eot_kw_enq_ms = 790;
  // An EOT ending a P300 session -> the next ENQ: the timed frames
  // re-established the session within 40 ms.
  uint32_t eot_session_enq_ms = 20;
  // A telegram -> the P300 06: 11 to 16, median 13; 16 00 00 -> 06 the same.
  uint32_t p300_ack_ms = 13;
  // The pause inside an error frame, after its 41 LL: 124 and 126.
  uint32_t p300_error_gap_ms = 125;
  // An answer -> the last moment a chained telegram is taken (VScotHO1_72:
  // answered at 911 ms, ignored from 954 ms).
  uint32_t chain_listen_ms = 930;
  // A pause inside a KW telegram that abandons it.
  uint32_t kw_byte_timeout_ms = 1000;
  // The 41 -> the rest (VScotHO1_72: 200 ms answered, 255 ms NACKed).
  uint32_t p300_start_gap_ms = 230;
  // A P300 session without traffic falls back to KW (0: never).
  uint32_t p300_idle_ms = 0;
  std::vector<uint8_t> rpc_answer{0x00};
  // P300 reads of these addresses, first..last, answer error 01.
  std::vector<std::pair<uint16_t, uint16_t>> unserved;

  static Quirks vitotronic_quirks() {
    Quirks q;
    // KW: the answer 19 to 30 after the last byte. P300: the answer frame 24
    // to 52 after the telegram, median 29.
    q.answer_delay_ms = 28;
    return q;
  }
};

class SimVitotronic : public SimPort {
 public:
  explicit SimVitotronic(const VitotronicConfig &config = {})
      : SimPort(config.quirks), c_(config), next_enq_at_(epoch_ + config.enq_first_ms) {
    memory.set(0x00F8, {0x20, 0xCB, 0x03, 0x51});  // VScotHO1_72 ident
  }

  SimMemory memory;
  SimMemory gfa;  // the burner control's memory (6B)

  // What the device is; a replay adjusts it from the log.
  VitotronicConfig &config() { return c_; }
  const VitotronicConfig &config() const { return c_; }
  bool in_p300() const { return mode_ == Mode::P300; }
  uint32_t nacks() const { return nacks_; }

 protected:
  void on_time(uint32_t now) override {
    if (mode_ == Mode::P300) {
      if (c_.p300_idle_ms != 0 && now - last_p300_ms_ > c_.p300_idle_ms && !this->output_pending()) {
        this->enter_kw(now);
      }
      return;
    }
    if (kw_ == Kw::ENQ_OPEN && now - enq_at_ > q_.enq_window_ms)
      kw_ = Kw::IDLE;
    if (kw_ != Kw::IDLE && kw_ != Kw::CHAIN)
      return;
    if (static_cast<int32_t>(now - next_enq_at_) < 0)
      return;
    // ENQs missed while the engine did not poll are not sent late: only the
    // latest one that is due.
    if (now - next_enq_at_ >= c_.enq_period_ms)
      next_enq_at_ += (now - next_enq_at_) / c_.enq_period_ms * c_.enq_period_ms;
    const uint32_t at = this->alive_from(next_enq_at_);
    if (at != next_enq_at_) {
      next_enq_at_ = at;
      if (static_cast<int32_t>(now - at) < 0)
        return;
    }
    this->send_enq(at);
    enq_at_ = at;
    kw_ = now - at > q_.enq_window_ms ? Kw::IDLE : Kw::ENQ_OPEN;
    next_enq_at_ = at + c_.enq_period_ms;
  }

  void on_byte(uint8_t b, uint32_t now) override {
    if (mode_ == Mode::P300) {
      this->p300_byte(b, now);
    } else {
      this->kw_byte(b, now);
    }
  }

 private:
  enum class Mode : uint8_t { KW, P300 };
  enum class Kw : uint8_t { IDLE, ENQ_OPEN, TELEGRAM, SYNC, CHAIN };
  enum class P3 : uint8_t { IDLE, FRAME, SYNC, SKIP };

  static constexpr uint8_t KW_READ = 0xF7;
  static constexpr uint8_t KW_WRITE = 0xF4;
  static constexpr uint8_t KW_GFA = 0x6B;
  static constexpr uint8_t SYNC_START = 0x16;
  static constexpr uint8_t PACKET_START = 0x41;
  // After a NACK, bytes closer together than this belong to the refused frame.
  static constexpr uint32_t SKIP_QUIET_MS = 50;

  void enter_kw(uint32_t now) {
    mode_ = Mode::KW;
    kw_ = Kw::IDLE;
    next_enq_at_ = now + c_.eot_session_enq_ms;
  }

  void kw_byte(uint8_t b, uint32_t now) {
    if ((kw_ == Kw::TELEGRAM || kw_ == Kw::SYNC) && now - last_rx_ms_ > c_.kw_byte_timeout_ms)
      kw_ = Kw::IDLE;
    if (kw_ == Kw::ENQ_OPEN && now - enq_at_ > q_.enq_window_ms)
      kw_ = Kw::IDLE;
    if (kw_ == Kw::CHAIN && static_cast<int32_t>(now - chain_until_) > 0)
      kw_ = Kw::IDLE;
    last_rx_ms_ = now;
    switch (kw_) {
      case Kw::IDLE:
        if (b == EOT)
          next_enq_at_ = now + c_.eot_kw_enq_ms;
        return;  // a 01 or a telegram outside an ENQ is ignored
      case Kw::ENQ_OPEN:
        if (b == ENQ_ACK) {
          kw_ = Kw::TELEGRAM;
          telegram_.clear();
        } else if (b == SYNC_START && c_.p300) {
          kw_ = Kw::SYNC;
          sync_zeros_ = 0;
        } else if (b == EOT) {
          kw_ = Kw::IDLE;
          next_enq_at_ = now + c_.eot_kw_enq_ms;
        }
        return;
      case Kw::CHAIN:
        if (b == EOT) {
          kw_ = Kw::IDLE;
          next_enq_at_ = now + c_.eot_kw_enq_ms;
          return;
        }
        if (q_.refuse_chain || b == ENQ_ACK)
          return;
        kw_ = Kw::TELEGRAM;
        telegram_.assign(1, b);
        this->kw_telegram_byte(now);
        return;
      case Kw::TELEGRAM:
        telegram_.push_back(b);
        this->kw_telegram_byte(now);
        return;
      case Kw::SYNC:
        if (b != 0x00) {
          kw_ = Kw::IDLE;
          return;
        }
        if (++sync_zeros_ == 2) {
          this->send(&ACK, 1, now + c_.p300_ack_ms);
          mode_ = Mode::P300;
          p3_ = P3::IDLE;
          last_p300_ms_ = now;
        }
        return;
    }
  }

  void kw_telegram_byte(uint32_t now) {
    if (telegram_.size() < 4)
      return;
    const uint8_t type = telegram_[0];
    const uint16_t address = static_cast<uint16_t>(telegram_[1] << 8 | telegram_[2]);
    const uint8_t length = telegram_[3];
    if (type == KW_WRITE && telegram_.size() < 4u + length)
      return;
    if (this->ignore_this()) {
      // Silence, then the next ENQ as after an answer: a VScotHO1_72 left 17
      // of 6066 telegrams unanswered and sent its next ENQ 2.97 s after each
      // (2026-11-21).
      kw_ = Kw::IDLE;
      next_enq_at_ = now + c_.post_answer_enq_ms;
      return;
    }
    std::vector<uint8_t> answer;
    if (type == KW_READ || type == KW_GFA) {
      answer.resize(length);
      (type == KW_GFA ? gfa : memory).read(address, length, answer.data());
    } else if (type == KW_WRITE) {
      for (uint8_t i = 0; i < length; ++i)
        memory.set(static_cast<uint16_t>(address + i), telegram_[4 + i]);
      answer.push_back(0x00);
    } else {
      kw_ = Kw::IDLE;  // an unknown type: no answer
      return;
    }
    const uint32_t end = this->send_answer(answer.data(), answer.size(), now);
    kw_ = Kw::CHAIN;
    chain_until_ = end + c_.chain_listen_ms;
    next_enq_at_ = end + c_.post_answer_enq_ms;
  }

  void p300_byte(uint8_t b, uint32_t now) {
    last_p300_ms_ = now;
    if (error_unacked_ && p3_ != P3::FRAME) {
      if (b == ACK) {
        error_unacked_ = false;
        return;
      }
      if (b == EOT) {
        // As logged on the VScotHO1_72: 06, and the session is over.
        error_unacked_ = false;
        this->send(&ACK, 1, now + c_.p300_ack_ms);
        mode_ = Mode::KW;
        kw_ = Kw::IDLE;
        next_enq_at_ = now + c_.enq_period_ms;
        return;
      }
    }
    if (p3_ == P3::SKIP) {
      // The rest of a refused frame, until the line has been quiet.
      if (now - skip_from_ms_ <= SKIP_QUIET_MS) {
        skip_from_ms_ = now;
        return;
      }
      p3_ = P3::IDLE;
    }
    switch (p3_) {
      case P3::IDLE:
      case P3::SKIP:
        if (b == EOT) {
          this->enter_kw(now);
        } else if (b == PACKET_START) {
          p3_ = P3::FRAME;
          frame_.clear();
          frame_start_ms_ = now;
        } else if (b == SYNC_START) {
          p3_ = P3::SYNC;
          sync_zeros_ = 0;
        }
        return;  // our own 06 after an answer, or noise
      case P3::SYNC:
        if (b == EOT) {
          this->enter_kw(now);
        } else if (b != 0x00) {
          p3_ = P3::IDLE;
        } else if (++sync_zeros_ == 2) {
          this->send(&ACK, 1, now + c_.p300_ack_ms);
          p3_ = P3::IDLE;
        }
        return;
      case P3::FRAME:
        if (frame_.empty() && c_.p300_start_gap_ms != 0 && now - frame_start_ms_ > c_.p300_start_gap_ms) {
          this->nack(now);
          return;
        }
        frame_.push_back(b);
        if (frame_[0] < 5) {
          this->nack(now);
          return;
        }
        if (frame_.size() == static_cast<std::size_t>(frame_[0]) + 2)
          this->p300_frame(now);
        return;
    }
  }

  void nack(uint32_t now) {
    ++nacks_;
    this->send(&NACK, 1, now + c_.p300_ack_ms);
    p3_ = P3::SKIP;
    skip_from_ms_ = now;
  }

  void p300_frame(uint32_t now) {
    const uint8_t ll = frame_[0];
    uint8_t sum = 0;
    for (uint8_t i = 0; i <= ll; ++i)
      sum = static_cast<uint8_t>(sum + frame_[i]);
    if (sum != frame_[ll + 1]) {
      this->nack(now);
      return;
    }
    p3_ = P3::IDLE;
    if (this->ignore_this())
      return;
    const uint8_t flags = frame_[2];
    const uint8_t function = flags & 0x1F;
    const uint16_t address = static_cast<uint16_t>(frame_[3] << 8 | frame_[4]);
    const uint8_t dl = frame_[5];
    const uint8_t payload = static_cast<uint8_t>(ll - 5);
    std::vector<uint8_t> body;  // LL onwards, without the checksum
    // DL is the error payload's length, the code's.
    auto error = [&](uint8_t code) { body = {6, 0x03, flags, frame_[3], frame_[4], 0x01, code}; };
    if (frame_[1] != 0x00) {
      error(0x05);
    } else if (function == 0x01) {
      if (this->unserved(address)) {
        error(0x01);
      } else {
        body = {static_cast<uint8_t>(5 + dl), 0x01, flags, frame_[3], frame_[4], dl};
        for (uint8_t i = 0; i < dl; ++i)
          body.push_back(memory.get(static_cast<uint16_t>(address + i)));
      }
    } else if (function == 0x02 && payload == dl) {
      for (uint8_t i = 0; i < dl; ++i)
        memory.set(static_cast<uint16_t>(address + i), frame_[6 + i]);
      body = {5, 0x01, flags, frame_[3], frame_[4], dl};
    } else if (function == 0x07) {
      body = {static_cast<uint8_t>(5 + c_.rpc_answer.size()), 0x01, flags, frame_[3], frame_[4],
              static_cast<uint8_t>(c_.rpc_answer.size())};
      body.insert(body.end(), c_.rpc_answer.begin(), c_.rpc_answer.end());
    } else {
      error(0x05);
    }
    if (!q_.swallow_ack)
      this->send(&ACK, 1, now + c_.p300_ack_ms);
    std::vector<uint8_t> out;
    out.push_back(PACKET_START);
    uint8_t cs = 0;
    for (uint8_t v : body) {
      out.push_back(v);
      cs = static_cast<uint8_t>(cs + v);
    }
    out.push_back(cs);
    const bool is_error = body[1] == 0x03;
    this->send_answer(out.data(), out.size(), now, is_error ? 2 : 0, c_.p300_error_gap_ms);
    error_unacked_ = is_error;
  }

  bool unserved(uint16_t address) const {
    for (const auto &range : c_.unserved) {
      if (address >= range.first && address <= range.second)
        return true;
    }
    return false;
  }

  VitotronicConfig c_;
  Mode mode_ = Mode::KW;
  Kw kw_ = Kw::IDLE;
  P3 p3_ = P3::IDLE;
  uint32_t next_enq_at_;
  uint32_t enq_at_ = 0;
  uint32_t chain_until_ = 0;
  uint32_t last_rx_ms_ = 0;
  uint32_t last_p300_ms_ = 0;
  uint32_t frame_start_ms_ = 0;
  uint32_t skip_from_ms_ = 0;
  uint8_t sync_zeros_ = 0;
  bool error_unacked_ = false;
  uint32_t nacks_ = 0;
  std::vector<uint8_t> telegram_;
  std::vector<uint8_t> frame_;
};

}  // namespace sim
