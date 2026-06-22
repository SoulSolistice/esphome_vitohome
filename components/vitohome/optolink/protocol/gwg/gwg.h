/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Cleanups vs. upstream: platform serial adapters and platform-gated
constructors removed (template<class C> ctor only); engine-level
_responseBuffer malloc + _expandResponseBuffer + _allocatedLength replaced
by a fixed std::array; request timeout lifted to a named constexpr member.
Bugfix: _tryOnResponse() now clears _currentDatapoint after the callback
(see THIRD_PARTY.md) so GWG is no longer one-shot.
*/

#pragma once

#include <array>
#include <functional>

#include "../../constants.h"
#include "../../datapoint/datapoint.h"
#include "../../helpers.h"
#include "../../interface/generic_interface.h"
#include "../../logging.h"
#include "packet_gwg.h"

namespace esphome {
namespace vitohome {
namespace optolink {

class GWGEngine {
 public:
  typedef std::function<void(const uint8_t *data, uint8_t length, const Datapoint &request)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, const Datapoint &request)> OnErrorCallback;

  // Named timeout (ms). GWG deliberately uses a 3000ms request watchdog,
  // distinct from VS2/VS1's 4000ms - value byte-identical to upstream.
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;

  // Fixed response buffer: bounds the largest GWG datapoint payload. 256 is a
  // safe upper bound (datapoint length is a uint8_t).
  static constexpr std::size_t kResponseBufferSize = 256;

  template <class C>
  explicit GWGEngine(C *interface)
      : _state(State::UNDEFINED),
        _currentMillis(optolink_millis()),
        _lastMillis(_currentMillis),
        _requestTime(0),
        _bytesTransferred(0),
        _interface(nullptr),
        _currentDatapoint(Datapoint(nullptr, 0x0000, 0, noconv)),
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
  GWGEngine(const GWGEngine &) = delete;
  GWGEngine &operator=(const GWGEngine &) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);

  bool read(const Datapoint &datapoint);
  bool write(const Datapoint &datapoint, const VariantValue &value);
  bool write(const Datapoint &datapoint, const uint8_t *data, uint8_t length);

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
  internals::SerialInterface *_interface;
  Datapoint _currentDatapoint;
  PacketGWG _currentRequest;
  std::array<uint8_t, kResponseBufferSize> _responseBuffer;
  OnResponseCallback _onResponseCallback;
  OnErrorCallback _onErrorCallback;

  inline void _setState(State state);

  void _init();
  void _send();
  void _receive();

  void _tryOnResponse();
  void _tryOnError(OptolinkResult result);
};

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
