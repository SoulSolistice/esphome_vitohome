/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
The time helper is renamed vw_millis() -> optolink_millis(); the host
std::chrono branch is kept so the native test harness builds, and is now
routed through an injectable clock (see optolink_host_clock below) so the
engine proofs can drive engine deadlines instead of racing them.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if defined(__linux__)
#include <chrono>  // NOLINT [build/c++11]

namespace esphome::vitohome::optolink::internals {

// Host-only injectable clock. The __linux__ branch of optolink_millis() exists
// solely for tests/native, and every engine here is a millis()-deadline state
// machine (GWGEngine::RESPONSE_TIMEOUT_MS, ENQ_VALIDITY_MS,
// VS1Engine::CHAIN_WINDOW_MS, the request watchdogs). A proof that advances
// those deadlines with std::this_thread::sleep_for is not testing the engine,
// it is betting that the host schedules it back within the tightest window in
// play -- 250 ms between two adjacent statements, in the GWG read case. That
// bet loses on a loaded machine: the engine then discards the "response" as
// stale, raises TIMEOUT, and the proof's assertions read whatever the PREVIOUS
// transaction left in its callback globals rather than failing on the quantity
// they name. Observed once as a 2-failure run of proof_gwg_access_mode.cpp
// (enq-age checks) under a concurrent sanitized fuzzing campaign, and
// reproduced exactly by injecting a 260 ms stall before the pump there.
//
// So a host proof takes the clock over instead: freeze() pins it at the
// current real reading, advance(ms) moves it by an exact amount, and the
// engine's deadline arithmetic becomes a pure function of the test script.
// Sleeps that only need to OVERSHOOT a deadline were merely slow; sleeps that
// had to land INSIDE one were the flake. Both become exact.
//
// Frozen time is deliberately NOT monotonically self-advancing: nothing may
// pass in an engine unless the test says so.
//
// Device builds never see any of this -- ESP_PLATFORM and Arduino keep their
// original one-line macros below.
struct HostClock {
  bool frozen;
  uint32_t now_ms;
};

inline HostClock &optolink_host_clock() {
  static HostClock clock{false, 0};
  return clock;
}

inline uint32_t optolink_host_real_millis() {
  // steady_clock, not system_clock: every use of this value is a difference
  // against an earlier reading, and system_clock can be stepped backwards by
  // NTP mid-run, which would make a deadline appear to have not yet started.
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::duration<uint32_t, std::milli>>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline uint32_t optolink_host_millis() {
  const HostClock &clock = optolink_host_clock();
  return clock.frozen ? clock.now_ms : optolink_host_real_millis();
}

}  // namespace esphome::vitohome::optolink::internals

// Test-side control surface. Freezing at the current real reading (rather than
// at 0) keeps the clock continuous across the takeover, so it is safe to call
// either side of constructing an engine -- an engine built before the freeze
// has already sampled a real millis() into its _currentMillis.
namespace esphome::vitohome::optolink {

inline void optolink_test_clock_freeze() {
  internals::HostClock &clock = internals::optolink_host_clock();
  if (!clock.frozen) {
    clock.now_ms = internals::optolink_host_real_millis();
    clock.frozen = true;
  }
}

inline void optolink_test_clock_advance(uint32_t ms) {
  // Takes the clock over if the caller has not already: advancing a clock that
  // is still running free would otherwise be a silent no-op, which is exactly
  // the failure mode this whole facility exists to remove.
  optolink_test_clock_freeze();
  internals::optolink_host_clock().now_ms += ms;  // wraparound is intentional: millis() wraps too
}

inline void optolink_test_clock_release() { internals::optolink_host_clock().frozen = false; }

}  // namespace esphome::vitohome::optolink

#define optolink_millis() ::esphome::vitohome::optolink::internals::optolink_host_millis()
#elif defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#define optolink_millis() (xTaskGetTickCount() * portTICK_PERIOD_MS)
#elif defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32)
#define optolink_millis() millis()
#else
#error "Unsupported target platform"
#endif

#define optolink_abort() abort()
