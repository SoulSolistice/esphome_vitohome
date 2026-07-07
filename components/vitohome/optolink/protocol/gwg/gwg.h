/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Cleanups vs. upstream: platform serial adapters and platform-gated
constructors removed (template<class C> ctor only); engine-level
_responseBuffer malloc + _expandResponseBuffer + _allocatedLength replaced
by a fixed std::array; request timeout lifted to a named constexpr member.
Bugfix: _tryOnResponse() clears the in-flight (busy) state after the
callback (see THIRD_PARTY.md) so GWG is no longer one-shot.
Sync poke: optional EOT (0x04) nudge while waiting for the device ENQ, gated by
GWGEngine::SEND_ENQ_POKE (default off, so the default build is unchanged); see
that flag for the vcontrold/VS1 rationale.
*/

#pragma once

#include <array>
#include <functional>

#include "../../constants.h"
#include "../../helpers.h"
#include "../../interface/generic_interface.h"
#include "../../logging.h"
#include "packet_gwg.h"

namespace esphome::vitohome::optolink {

class GWGEngine {
 public:
  // Byte-mover API (see vs2.h). GWG carries no address in the response, so the
  // engine echoes the request address back to the caller unchanged.
  typedef std::function<void(const uint8_t* data, uint8_t length, uint16_t address)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, uint16_t address)> OnErrorCallback;

  // Named timeout (ms). GWG deliberately uses a 3000ms request watchdog,
  // distinct from VS2/VS1's 4000ms - value byte-identical to upstream.
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;

  // Fixed response buffer: bounds the largest GWG datapoint payload. 256 is a
  // safe upper bound (datapoint length is a uint8_t).
  static constexpr std::size_t kResponseBufferSize = 256;

  // Active sync poke. OFF by default: INIT waits passively for the device's ENQ
  // (0x05), byte-identical to the original behaviour. Set to true only if a GWG
  // device never establishes -- then INIT also sends an EOT (0x04) every
  // ENQ_POKE_INTERVAL_MS while waiting, mirroring vcontrold's GWG sync
  // (SEND 04; WAIT 05) and the VS1 engine's EOT fallback. Needs a real GWG unit
  // to validate; GWG is an untested protocol.
  static constexpr bool SEND_ENQ_POKE = false;
  static constexpr uint32_t ENQ_POKE_INTERVAL_MS = 3000;

  template <class C>
  explicit GWGEngine(C* interface)
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
      optolink_log_e("Could not create serial interface");
      optolink_abort();
    }
  }
  ~GWGEngine();
  GWGEngine(const GWGEngine&) = delete;
  GWGEngine& operator=(const GWGEngine&) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);

  bool read(uint16_t address, uint8_t length);
  bool write(uint16_t address, const uint8_t* data, uint8_t length);

  bool begin();
  void loop();
  void end();

  int getState() const;
  bool isBusy() const;

 private:
  enum class State { INIT, SEND, RECEIVE, UNDEFINED } _state;
  uint32_t _currentMillis;
  uint32_t _lastMillis;
  uint32_t _requestTime;
  uint8_t _bytesTransferred;
  internals::SerialInterface* _interface;
  uint16_t _currentAddress;
  uint8_t _currentLength;
  bool _busy;
  PacketGWG _currentRequest;
  std::array<uint8_t, kResponseBufferSize> _responseBuffer;
  OnResponseCallback _onResponseCallback;
  OnErrorCallback _onErrorCallback;

  inline void _setState(State state);

  void _init();
  void _send();
  void _receive();

  void _tryOnResponse(uint8_t length);
  void _tryOnError(OptolinkResult result);
};

}  // namespace esphome::vitohome::optolink
