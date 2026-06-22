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
*/

#pragma once

#include <array>
#include <functional>

#include "../../logging.h"
#include "../../constants.h"
#include "../../helpers.h"
#include "packet_vs1.h"
#include "../../datapoint/datapoint.h"
#include "../../interface/generic_interface.h"

namespace esphome {
namespace vitohome {
namespace optolink {

class VS1Engine {
 public:
  typedef std::function<void(const uint8_t *data, uint8_t length, const Datapoint &request)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, const Datapoint &request)> OnErrorCallback;

  // Named timeouts (ms). Values byte-identical to the previous inline
  // literals; kept per-engine (do not unify across protocols).
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 4000;       // per-request response watchdog
  static constexpr uint32_t ENQ_RESET_INTERVAL_MS = 3000;    // reset/EOT when no ENQ (Vitotronic on VS2)
  static constexpr uint32_t SYNC_WINDOW_MS = 50;             // ENQ-ACK / re-send sync window

  // Fixed response buffer: bounds the largest VS1 datapoint payload. 256 is a
  // safe upper bound (datapoint length is a uint8_t).
  static constexpr std::size_t kResponseBufferSize = 256;

  template <class C>
  explicit VS1Engine(C *interface)
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
  ~VS1Engine();
  VS1Engine(const VS1Engine &) = delete;
  VS1Engine &operator=(const VS1Engine &) = delete;

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
  enum class State { INIT, SYNC_ENQ, SYNC_RECV, SEND, RECEIVE, UNDEFINED } _state;
  uint32_t _currentMillis;
  uint32_t _lastMillis;
  uint32_t _requestTime;
  uint8_t _bytesTransferred;
  internals::SerialInterface *_interface;
  Datapoint _currentDatapoint;
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

  void _tryOnResponse();
  void _tryOnError(OptolinkResult result);
};

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
