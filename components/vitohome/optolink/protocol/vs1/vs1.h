/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Cleanups vs. upstream: platform serial adapters and platform-gated
constructors removed (only the template<class C> ctor remains); the
engine-level _responseBuffer malloc + _expandResponseBuffer + _allocatedLength
is replaced by a fixed std::array; inline timeouts lifted to named
constexpr members (values unchanged), including the two 50ms sync windows.
Fail-soft: the constructor no longer abort()s on serial-interface allocation
failure; it leaves the engine in a failed state and begin() returns false (see
THIRD_PARTY.md), so the wrapper mark_failed()s instead of the firmware aborting.
*/

#pragma once

#include <array>
#include <cassert>
#include <functional>

#include "../../constants.h"
#include "../../helpers.h"
#include "../../interface/generic_interface.h"
#include "../../logging.h"
#include "packet_vs1.h"

namespace esphome::vitohome::optolink {

class VS1Engine {
 public:
  // Byte-mover API (see vs2.h). KW/VS1 carries no address in the response, so
  // the engine echoes the request address back to the caller unchanged.
  typedef std::function<void(const uint8_t *data, uint8_t length, uint16_t address)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, uint16_t address)> OnErrorCallback;

  // Named timeouts (ms). Values byte-identical to the previous inline
  // literals; kept per-engine (do not unify across protocols).
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 4000;     // per-request response watchdog
  static constexpr uint32_t ENQ_RESET_INTERVAL_MS = 3000;  // reset/EOT when no ENQ (Vitotronic on VS2)
  // Two windows, deliberately separate. They used to be one 50 ms
  // SYNC_WINDOW_MS serving both, which conflated a protocol requirement with a
  // throughput policy and pinned the second to the first.
  //
  // ENQ_ACK_WINDOW_MS: how long after the device's ENQ we may still answer it
  // with ENQ_ACK and start a telegram (_syncEnq). This one IS a protocol
  // constraint -- the KW-family master has to answer inside the device's sync
  // window -- and keeps upstream's value.
  static constexpr uint32_t ENQ_ACK_WINDOW_MS = 50;
  // CHAIN_WINDOW_MS: how long a chain of telegrams may pause between an answer
  // and the next request before the engine gives up and waits for a fresh ENQ
  // (_syncRecv). This is a throughput policy, not a protocol constraint, and
  // the two have very different natural sizes.
  //
  // Hardware-measured (2026-08-24 capture, VScotHO1_72 0x20CB, ESP32-C3, 56
  // entities): the answer-to-next-request gap ran 22-42 ms over 235
  // transactions (median 29, p90 35). Against the old 50 ms that is ~8 ms of
  // headroom -- fine on that config, but any extra loop latency pushes a gap
  // past it and silently ends the chain, dropping the poll rate back to one
  // telegram per ENQ (~1.2-2 s each). vogod uses 1500 ms for exactly this job,
  // and GWGEngine::ENQ_VALIDITY_MS is 1500 ms; this matches them.
  //
  // Exceeding it is a SOFT failure: the chain simply ends and the next
  // telegram waits for an ENQ. So the cost of being too tight is throughput,
  // and the cost of being too loose is bounded by the guard in _syncRecv().
  static constexpr uint32_t CHAIN_WINDOW_MS = 1500;
  // GFA_GAP_MS: how long a GFA read (gfaRead(), type 0x6B) chained onto an
  // answer waits after that answer before it goes out. The read is forwarded
  // to the burner control, a second controller behind the Vitotronic. The
  // hardware-confirmed series (2026-10-26, VScotHO1_72, burner control 0x24)
  // sent every 0x6B telegram 25 ms after the previous answer and got an answer
  // to all of them; without the pause it has never been tried. A GFA read that
  // opens a chain goes out on the device's ENQ like any other, the bus having
  // been idle since. The cost is 25 ms per chained GFA read.
  static constexpr uint32_t GFA_GAP_MS = 25;
  static_assert(GFA_GAP_MS < CHAIN_WINDOW_MS, "the GFA pause must fit inside the chain window");
  // ANSWER_TIMEOUT_MS: how long the line may stay silent while an answer is
  // due -- before its first byte and between bytes. Answers take 13-81 ms on
  // VScotHO1_72 (KW); a telegram the controller ignores is followed by its
  // next ENQ about 2 s later, which must not be read as data
  // (2026-10-28, 01 7B 08 00 02 over the P300 detour). Well under that, well
  // over any answer.
  static constexpr uint32_t ANSWER_TIMEOUT_MS = 1000;

  // Fixed response buffer: bounds the largest VS1 datapoint payload. 256 is a
  // safe upper bound (datapoint length is a uint8_t).
  static constexpr std::size_t kResponseBufferSize = 256;

  template<class C>
  explicit VS1Engine(C *interface)
      : _state(State::UNDEFINED),
        _currentMillis(optolink_millis()),
        _lastMillis(_currentMillis),
        _requestTime(0),
        _bytesTransferred(0),
        _interface(nullptr),
        _currentAddress(0),
        _currentLength(0),
        _busy(false),
        _currentRequest(),
        _responseBuffer{},
        _onResponseCallback(nullptr),
        _onErrorCallback(nullptr) {
    assert(interface != nullptr);
    _interface = new (std::nothrow) internals::GenericInterface<C>(interface);
    if (!_interface) {
      // Fail soft on allocation failure: leave _interface null and let the
      // object be constructed in a failed state. begin() then returns false and
      // the ESPHome wrapper mark_failed()s the component (see
      // VitoHomeComponent::setup), rather than abort()ing the whole firmware on
      // a transient OOM. The vendored engine has no ESPHome dependency, so it
      // signals failure through begin()'s return value, not mark_failed().
      optolink_log_e("Could not create serial interface");
    }
  }
  ~VS1Engine();
  VS1Engine(const VS1Engine &) = delete;
  VS1Engine &operator=(const VS1Engine &) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);

  bool read(uint16_t address, uint8_t length);
  bool write(uint16_t address, const uint8_t *data, uint8_t length);
  // A read in the burner control's address space (type 0x6B, Vitosoft's
  // GFA_READ; see PacketVS1Type). The answer arrives through onResponse like a
  // read's: <length> raw bytes. There is no GFA write.
  bool gfaRead(uint16_t address, uint8_t length);
  // Any KW telegram: `type addr-hi addr-lo length`, plus `length` payload
  // bytes when `data` is given. A telegram without payload is answered with
  // `length` raw bytes, one with payload with a single acknowledgement byte
  // (the shape of 0xF4; hardware-confirmed for it, assumed for other types).
  // read(), write() and gfaRead() are this with 0xF7, 0xF4 and 0x6B; the raw
  // lane sends any other type byte through it
  // (VitoHomeComponent::queue_raw_op()).
  bool telegram(uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr);

  bool begin();
  void loop();
  void end();

  bool isBusy() const;

 private:
  enum class State { INIT, SYNC_ENQ, SYNC_RECV, SEND, RECEIVE, UNDEFINED } _state;
  uint32_t _currentMillis;
  uint32_t _lastMillis;
  uint32_t _requestTime;
  uint8_t _bytesTransferred;
  internals::SerialInterface *_interface;
  uint16_t _currentAddress;
  uint8_t _currentLength;
  bool _busy;
  PacketVS1 _currentRequest;
  std::array<uint8_t, kResponseBufferSize> _responseBuffer;
  OnResponseCallback _onResponseCallback;
  OnErrorCallback _onErrorCallback;

  inline void _setState(State state);

  void _init();
  void _syncEnq();
  void _syncRecv();
  void _send();
  void _receive();

  void _tryOnResponse(uint8_t length);
  void _tryOnError(OptolinkResult result);
};

}  // namespace esphome::vitohome::optolink
