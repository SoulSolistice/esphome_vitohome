#pragma once
#include <cstddef>
#include <cstdint>

#include "esphome/components/uart/uart.h"

namespace esphome {
namespace vitohome {

// Adapter satisfying the optolink engine's generic-interface contract against ESPHome's
// uart::UARTDevice.
//
// the optolink engine calls these from inside its own loop(), so we must remain
// non-blocking. ESPHome's UART API is already non-blocking, so each method is
// a thin forward.
//
// Lifetime: ESPHomeUARTInterface stores a non-owning pointer to a UARTDevice
// that outlives it. The component holds both as members in the correct order.
class ESPHomeUARTInterface {
 public:
  explicit ESPHomeUARTInterface(uart::UARTDevice *dev) : dev_(dev) {}

  bool begin() {
    // ESPHome configures and opens the UART from YAML; nothing for us to do
    // beyond a sanity check.
    return dev_ != nullptr;
  }

  void end() {
    // ESPHome owns the UART lifecycle.
  }

  std::size_t write(const uint8_t *data, uint8_t length) {
    if (dev_ == nullptr || data == nullptr || length == 0) return 0;
    dev_->write_array(data, length);
    return length;  // ESPHome's write_array does not signal partial writes
  }

  uint8_t read() {
    // the optolink engine only calls read() after available() > 0, so the byte is
    // guaranteed to be there. We still guard defensively.
    uint8_t b = 0;
    if (dev_ != nullptr) {
      dev_->read_byte(&b);
    }
    return b;
  }

  std::size_t available() {
    if (dev_ == nullptr) return 0;
    int n = dev_->available();
    return n > 0 ? static_cast<std::size_t>(n) : 0;
  }

 private:
  uart::UARTDevice *dev_;
};

}  // namespace vitohome
}  // namespace esphome
