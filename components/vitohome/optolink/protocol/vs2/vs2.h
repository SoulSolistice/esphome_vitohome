/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Cleanups vs. upstream: the platform serial adapters (HardwareSerial /
SoftwareSerial / Linux) and their gated constructors are removed; only the
duck-typed template<class C> constructor remains (vitohome constructs the
engine through its own ESPHomeUARTInterface). The inline state-machine
timeouts are lifted to named constexpr members (values unchanged).
*/

#pragma once

#include <functional>

#include "../../constants.h"
#include "../../datapoint/datapoint.h"
#include "../../helpers.h"
#include "../../interface/generic_interface.h"
#include "../../logging.h"
#include "parser_vs2.h"

namespace esphome {
namespace vitohome {
namespace optolink {

class VS2Engine {
 public:
  typedef std::function<void(const PacketVS2& response, const Datapoint& request)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, const Datapoint& request)> OnErrorCallback;

  // Named timeouts (ms). Values are byte-identical to the previous inline
  // literals; kept per-engine (do not unify across protocols).
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 4000;     // per-request response watchdog
  static constexpr uint32_t HANDSHAKE_RETRY_MS = 3000;     // RESET-ACK / INIT-ACK window
  static constexpr uint32_t KEEPALIVE_INTERVAL_MS = 3000;  // idle re-INIT keepalive

  template <class C>
  explicit VS2Engine(C* interface)
      : _state(State::UNDEFINED),
        _currentMillis(optolink_millis()),
        _lastMillis(_currentMillis),
        _requestTime(0),
        _bytesTransferred(0),
        _interface(nullptr),
        _parser(),
        _currentDatapoint(Datapoint(nullptr, 0, 0, noconv)),
        _currentPacket(),
        _onResponseCallback(nullptr),
        _onErrorCallback(nullptr) {
    assert(interface != nullptr);
    _interface = new (std::nothrow) internals::GenericInterface<C>(interface);
    if (!_interface) {
      optolink_log_e("Could not create serial interface");
      optolink_abort();
    }
  }
  ~VS2Engine();
  VS2Engine(const VS2Engine&) = delete;
  VS2Engine& operator=(const VS2Engine&) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);

  bool read(const Datapoint& datapoint);
  bool write(const Datapoint& datapoint, const VariantValue& value);
  bool write(const Datapoint& datapoint, const uint8_t* data, uint8_t length);

  bool begin();
  void loop();
  void end();

  int getState() const;
  bool isBusy() const;

 private:
  enum class State {
    RESET,
    RESET_ACK,
    INIT,
    INIT_ACK,
    IDLE,
    SENDSTART,
    SENDPACKET,
    SEND_CRC,
    SEND_ACK,
    RECEIVE,
    RECEIVE_ACK,
    UNDEFINED
  } _state;
  uint32_t _currentMillis;
  uint32_t _lastMillis;
  uint32_t _requestTime;
  uint8_t _bytesTransferred;
  internals::SerialInterface* _interface;
  internals::ParserVS2 _parser;
  Datapoint _currentDatapoint;
  PacketVS2 _currentPacket;
  OnResponseCallback _onResponseCallback;
  OnErrorCallback _onErrorCallback;

  inline void _setState(State state);

  void _reset();
  void _resetAck();
  void _init();
  void _initAck();
  void _idle();
  void _sendStart();
  void _sendPacket();
  void _sendCRC();
  void _sendAck();
  void _receive();
  void _receiveAck();

  void _tryOnResponse();
  void _tryOnError(OptolinkResult result);
};

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
