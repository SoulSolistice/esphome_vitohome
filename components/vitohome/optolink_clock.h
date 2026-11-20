// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_CLOCK_H
#define VITOHOME_OPTOLINK_CLOCK_H

// The engine's clock: optolink_millis(), in ms, wrapping like millis(). On a
// device the platform's tick; on the host an injectable clock, so the native
// tests set every deadline exactly instead of sleeping past it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if defined(__linux__)
#include <chrono>  // NOLINT [build/c++11]

namespace esphome::vitohome::optolink::internals {

// Host only. Every engine is a millis()-deadline state machine, so a test
// that sleeps is betting the host schedules it back inside the tightest
// window in play, 250 ms in a GWG read. A frozen clock moves only when the
// test says so, which makes each deadline a function of the test script.
struct HostClock {
  bool frozen;
  uint32_t now_ms;
};

inline HostClock &optolink_host_clock() {
  static HostClock clock{false, 0};
  return clock;
}

inline uint32_t optolink_host_real_millis() {
  // steady_clock: every use is a difference against an earlier reading, which
  // a system_clock stepped by NTP would break.
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::duration<uint32_t, std::milli>>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

inline uint32_t optolink_host_millis() {
  const HostClock &clock = optolink_host_clock();
  return clock.frozen ? clock.now_ms : optolink_host_real_millis();
}

}  // namespace esphome::vitohome::optolink::internals

// What a test controls. freeze() pins the clock at the current real reading,
// so it may come before or after an engine is built.
namespace esphome::vitohome::optolink {

inline void optolink_test_clock_freeze() {
  internals::HostClock &clock = internals::optolink_host_clock();
  if (!clock.frozen) {
    clock.now_ms = internals::optolink_host_real_millis();
    clock.frozen = true;
  }
}

// Freezes the clock first if it runs free, so an advance is never a no-op.
inline void optolink_test_clock_advance(uint32_t ms) {
  optolink_test_clock_freeze();
  internals::optolink_host_clock().now_ms += ms;  // wraps, as millis() does
}

inline void optolink_test_clock_release() { internals::optolink_host_clock().frozen = false; }

}  // namespace esphome::vitohome::optolink

#define optolink_millis() ::esphome::vitohome::optolink::internals::optolink_host_millis()
#elif defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#define optolink_millis() (xTaskGetTickCount() * portTICK_PERIOD_MS)
#elif defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32)
// Included here: the optolink library's sources are compiled standalone, so no
// ESPHome header brings Arduino.h. Reached on ESP8266 only; Arduino on ESP32
// defines ESP_PLATFORM and takes the branch above.
#include <Arduino.h>
#define optolink_millis() millis()
#else
#error "Unsupported target platform"
#endif

#endif  // VITOHOME_OPTOLINK_CLOCK_H
