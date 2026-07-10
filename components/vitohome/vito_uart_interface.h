#pragma once
#include <cstddef>
#include <cstdint>

#include "esphome/components/uart/uart.h"
#ifdef VITOHOME_LOG_FRAMES
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#endif

namespace esphome::vitohome {

// Adapter satisfying the optolink engine's generic-interface contract against ESPHome's
// uart::UARTDevice.
//
// the optolink engine calls these from inside its own loop(), so we must remain
// non-blocking. ESPHome's UART API is already non-blocking, so each method is
// a thin forward.
//
// Lifetime: ESPHomeUARTInterface stores a non-owning pointer to a UARTDevice
// that outlives it. The component holds both as members in the correct order.
//
// --- optional frame logging (-DVITOHOME_LOG_FRAMES, `log_frames: true`) ------
// The obvious way to see the wire is ESPHome's own `uart: debug:` block, but it
// has no idea where an Optolink frame starts or ends: the user has to pick an
// `after:` rule, and the recipe that circulates for Optolink (`delimiter:
// [0x06]`) is wrong on KW. 0x06 is the P300 ACK; on KW it is an ordinary data
// byte, so the splitter tears frames apart whenever a payload byte happens to
// be 0x06 -- hardware-observed on VScotHO1_72 (device 0x20CB), where both the
// clock's BCD hour and the low byte of address 0x2306 split a frame mid-
// telegram in the 2026-07-09 logs.
//
// This adapter reconstructs the real boundaries and needs no delimiter. BOTH
// directions are buffered and flushed on the first of: traffic in the opposite
// direction, an inter-byte gap longer than FRAME_GAP_MS, or a full buffer.
//
// Buffering TX is not optional. An earlier revision assumed "one write() == one
// telegram, on every protocol". That is true for VS1/KW and GWG, and FALSE for
// VS2/P300: VS2Engine::_sendStart(), _sendPacket() and _sendCRC() are three
// separate states of its send machine, each issuing its own write() one loop()
// apart, so a single request reached the log as three lines --
// hardware-observed on VScotHO1_72 (2026-07-10, P300):
//     >>> 41                       (PACKETSTART)
//     >>> 05:00:01:73:62:28        (body)
//     >>> 03                       (checksum)
// Buffering reassembles that into one `>>> 41:05:00:01:73:62:28:03`.
//
// FRAME_GAP_MS = 30 is chosen from the wire, not from taste. At 4800 8E2 one
// byte occupies 11 bits = 2.29 ms. Measured on P300: the gap between the three
// TX pieces of one telegram is 18-20 ms, while the gap between the master's ACK
// (0x06) and the next telegram's PACKETSTART is ~35 ms. 30 ms therefore joins a
// telegram and separates the ACK from it. It is also >13 byte-times, far above
// the intra-frame spacing, and far below KW's ~2.2 s idle-sync cadence.
//
// Everything below is compiled out entirely when the flag is absent: no buffers,
// no timestamps, no per-byte branch. frame_tick() degrades to an empty inline.
class ESPHomeUARTInterface {
 public:
  explicit ESPHomeUARTInterface(uart::UARTDevice* dev) : dev_(dev) {}

  bool begin() {
    // ESPHome configures and opens the UART from YAML; nothing for us to do
    // beyond a sanity check.
    return dev_ != nullptr;
  }

  void end() {
    // ESPHome owns the UART lifecycle.
  }

  std::size_t write(const uint8_t* data, uint8_t length) {
    if (dev_ == nullptr || data == nullptr || length == 0) return 0;
#ifdef VITOHOME_LOG_FRAMES
    // Traffic in the other direction closes the pending frame, so the log reads
    // as a strict request/response alternation.
    this->flush_rx_();
    for (uint8_t i = 0; i < length; i++) this->note_tx_(data[i]);
#endif
    dev_->write_array(data, length);
    return length;  // ESPHome's write_array does not signal partial writes
  }

  uint8_t read() {
    // the optolink engine only calls read() after available() > 0, so the byte is
    // guaranteed to be there. We still guard defensively.
    uint8_t b = 0;
    if (dev_ != nullptr) {
      dev_->read_byte(&b);
#ifdef VITOHOME_LOG_FRAMES
      this->flush_tx_();
      this->note_rx_(b);
#endif
    }
    return b;
  }

  std::size_t available() {
    if (dev_ == nullptr) return 0;
    int n = dev_->available();
    return n > 0 ? static_cast<std::size_t>(n) : 0;
  }

  // Called from the hub's loop(). Closes whichever frame has gone quiet for
  // FRAME_GAP_MS. No-op unless frame logging is compiled in.
  void frame_tick() {
#ifdef VITOHOME_LOG_FRAMES
    const uint32_t now = millis();
    if (this->tx_len_ != 0 && now - this->tx_last_ms_ >= FRAME_GAP_MS) this->flush_tx_();
    if (this->rx_len_ != 0 && now - this->rx_last_ms_ >= FRAME_GAP_MS) this->flush_rx_();
#endif
  }

  static constexpr bool frame_logging_enabled() {
#ifdef VITOHOME_LOG_FRAMES
    return true;
#else
    return false;
#endif
  }

 private:
  uart::UARTDevice* dev_;

#ifdef VITOHOME_LOG_FRAMES
  // The longest single telegram this component moves is a 37-byte read (the
  // P300 single-telegram limit) plus protocol framing; 64 covers it with
  // headroom, and a full buffer flushes early rather than truncating.
  static constexpr uint8_t BUF_SIZE = 64;
  static constexpr uint32_t FRAME_GAP_MS = 30;
  static constexpr const char* TAG_FRAMES = "vitohome.frames";

  void note_rx_(uint8_t b) {
    if (this->rx_len_ >= BUF_SIZE) this->flush_rx_();
    this->rx_buf_[this->rx_len_++] = b;
    this->rx_last_ms_ = millis();
  }

  void flush_rx_() {
    if (this->rx_len_ == 0) return;
    log_frame_("<<<", this->rx_buf_, this->rx_len_);
    this->rx_len_ = 0;
  }

  void note_tx_(uint8_t b) {
    if (this->tx_len_ >= BUF_SIZE) this->flush_tx_();
    this->tx_buf_[this->tx_len_++] = b;
    this->tx_last_ms_ = millis();
  }

  void flush_tx_() {
    if (this->tx_len_ == 0) return;
    log_frame_(">>>", this->tx_buf_, this->tx_len_);
    this->tx_len_ = 0;
  }

  static void log_frame_(const char* dir, const uint8_t* data, uint8_t length) {
    // 3 chars per byte ("XX:") plus the NUL.
    char line[3 * BUF_SIZE + 1];
    std::size_t pos = 0;
    for (uint8_t i = 0; i < length && pos + 4 <= sizeof(line); i++) {
      int n = snprintf(&line[pos], sizeof(line) - pos, "%s%02X", i != 0 ? ":" : "", data[i]);
      if (n <= 0) break;
      pos += static_cast<std::size_t>(n);
    }
    line[pos] = '\0';
    ESP_LOGD(TAG_FRAMES, "%s %s", dir, line);
  }

  uint8_t rx_buf_[BUF_SIZE]{};
  uint8_t tx_buf_[BUF_SIZE]{};
  uint8_t rx_len_{0};
  uint8_t tx_len_{0};
  uint32_t rx_last_ms_{0};
  uint32_t tx_last_ms_{0};
#endif
};

}  // namespace esphome::vitohome
