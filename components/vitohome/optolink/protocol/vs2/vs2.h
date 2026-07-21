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

#include <cassert>
#include <functional>

#include "../../constants.h"
#include "../../helpers.h"
#include "../../interface/generic_interface.h"
#include "../../logging.h"
#include "parser_vs2.h"

namespace esphome::vitohome::optolink {

class VS2Engine {
 public:
  // Byte-mover API: the engine moves raw payloads over the wire and knows
  // nothing about datapoints, converters or scaling. A response is surfaced as
  // (data, length, address); on P300 `address` is the one ECHOED in the
  // response frame (a real wire-level datum), so a caller can match it against
  // the request it dispatched. Correlation of a response to its originating
  // request is the caller's job (the engine is strictly single-in-flight).
  typedef std::function<void(const uint8_t *data, uint8_t length, uint16_t address)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, uint16_t address)> OnErrorCallback;

  // Named timeouts (ms). Values are byte-identical to the previous inline
  // literals; kept per-engine (do not unify across protocols).
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 4000;     // per-request response watchdog
  static constexpr uint32_t HANDSHAKE_RETRY_MS = 3000;     // RESET-ACK / INIT-ACK window
  static constexpr uint32_t KEEPALIVE_INTERVAL_MS = 3000;  // idle re-INIT keepalive

  template<class C>
  explicit VS2Engine(C *interface)
      : _state(State::UNDEFINED),
        _currentMillis(optolink_millis()),
        _lastMillis(_currentMillis),
        _requestTime(0),
        _bytesTransferred(0),
        _interface(nullptr),
        _parser(),
        _currentAddress(0),
        _currentLength(0),
        _busy(false),
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
  VS2Engine(const VS2Engine &) = delete;
  VS2Engine &operator=(const VS2Engine &) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);

  bool read(uint16_t address, uint8_t length);
  bool write(uint16_t address, const uint8_t *data, uint8_t length);

  bool begin();
  void loop();
  void end();

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
  internals::SerialInterface *_interface;
  internals::ParserVS2 _parser;
  // In-flight request context (no Datapoint): the address is echoed back to
  // the caller, the length is retained for symmetry with the byte-oriented
  // engines, and _busy is the single-in-flight guard.
  uint16_t _currentAddress;
  uint8_t _currentLength;
  bool _busy;
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

}  // namespace esphome::vitohome::optolink
