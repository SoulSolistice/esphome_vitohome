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
Fail-soft: the constructor no longer abort()s on serial-interface allocation
failure; it leaves the engine in a failed state and begin() returns false (see
THIRD_PARTY.md), so the wrapper mark_failed()s instead of the firmware aborting.
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
      // Fail soft on allocation failure: leave _interface null and let the
      // object be constructed in a failed state. begin() then returns false and
      // the ESPHome wrapper mark_failed()s the component (see
      // VitoHomeComponent::setup), rather than abort()ing the whole firmware on
      // a transient OOM. The vendored engine has no ESPHome dependency, so it
      // signals failure through begin()'s return value, not mark_failed().
      optolink_log_e("Could not create serial interface");
    }
  }
  ~VS2Engine();
  VS2Engine(const VS2Engine &) = delete;
  VS2Engine &operator=(const VS2Engine &) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);

  bool read(uint16_t address, uint8_t length);
  bool write(uint16_t address, const uint8_t *data, uint8_t length);
  // Remote procedure call (FunctionCode::RPC, 0x07): `parameter` goes out as
  // the request's payload and `length` is its size (0 for a procedure without
  // one). The answer arrives through onResponse like a read's, as long as the
  // controller makes it. P300 only: KW and GWG have no such telegram.
  bool rpc(uint16_t address, const uint8_t *parameter, uint8_t length);
  // A read in the burner control's address space: the KW telegram 0x6B
  // (PacketVS1Type.GFA_READ), which a controller refuses as a P300 function
  // code. So the engine drops the link to KW for it -- EOT, the controller's
  // ENQ, `01 6B <addr> <len>`, <len> raw bytes back -- chains further GFA reads
  // onto that KW session, and re-establishes P300 (EOT, ENQ, SYNC) once the
  // next request is not a GFA read or none follows. The answer arrives through
  // onResponse like a read's, with the request's address. At most
  // GFA_MAX_LENGTH bytes.
  bool gfaRead(uint16_t address, uint8_t length);
  static constexpr uint8_t GFA_MAX_LENGTH = 64;
  // How long the KW session waits for the controller's ENQ after the EOT that
  // drops P300 (the controller sends one about every 2 s when idle), and,
  // once a GFA answer is in, for another GFA read before returning to P300.
  // GFA_GAP_MS is the pause before a chained GFA telegram, as on KW
  // (VS1Engine::GFA_GAP_MS).
  static constexpr uint32_t GFA_ENQ_TIMEOUT_MS = HANDSHAKE_RETRY_MS;
  static constexpr uint32_t GFA_LINGER_MS = 250;
  static constexpr uint32_t GFA_GAP_MS = 25;

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
    // The KW detour of a GFA read (gfaRead()).
    GFA_DROP,     // EOT: leave P300
    GFA_ENQ,      // wait for the controller's ENQ
    GFA_SEND,     // [01] 6B <addr hi> <addr lo> <len>
    GFA_RECEIVE,  // <len> raw bytes
    GFA_CHAIN,    // another GFA read, or back to P300
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
  // A GFA read in flight (gfaRead()): its KW telegram, with the ENQ_ACK in
  // front when it opens the KW session, and its answer.
  bool _gfa{false};
  uint8_t _gfaFrame[5]{};
  uint8_t _gfaFrameLen{0};
  uint8_t _gfaAnswer[GFA_MAX_LENGTH]{};
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
  void _gfaDrop();
  void _gfaEnq();
  void _gfaSend();
  void _gfaReceive();
  void _gfaChain();
  void _gfaStart(bool enqAck);

  void _tryOnResponse();
  void _tryOnError(OptolinkResult result);
};

}  // namespace esphome::vitohome::optolink
