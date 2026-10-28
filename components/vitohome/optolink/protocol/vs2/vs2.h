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
  // Any P300 function code (1..0x1F, the low five bits of the flags byte):
  // without `data` the frame of a read (DataLength = `length`), with it
  // `length` payload bytes. read(), write() and rpc() are this with 0x01,
  // 0x02 and 0x07. The answer -- a RESPONSE frame echoing the code --
  // arrives through onResponse; for a code other than READ/WRITE its payload
  // is as long as the frame says. An ERROR frame raises DEVICE_ERROR and its
  // payload, the controller's error code, is kept for errorPayload(). For the
  // raw lane (VitoHomeComponent::queue_raw_op()).
  bool request(uint8_t function, uint16_t address, uint8_t length, const uint8_t *data = nullptr);
  // The payload of the last ERROR frame (at most ERROR_PAYLOAD_MAX bytes,
  // length 0 when the last request got none): `01` for an address the
  // controller does not serve, `05` for a function code it lacks
  // (VScotHO1_72, 2026-10-27).
  const uint8_t *errorPayload(uint8_t &length) const;
  static constexpr uint8_t ERROR_PAYLOAD_MAX = 4;
  // A KW telegram over a P300 link: `type addr-hi addr-lo length`, plus
  // `length` payload bytes when `data` is given. The controller refuses KW
  // telegrams as P300 function codes (0x6B: an ERROR frame, code 05), so the
  // engine drops the link to KW for it -- EOT, the controller's ENQ,
  // `01 <telegram>`, <length> raw bytes back (one acknowledgement byte after a
  // telegram with payload) -- chains further KW telegrams onto that session,
  // and re-establishes P300 (EOT, ENQ, SYNC) once the next request is not one
  // or none follows. The answer arrives through onResponse with the request's
  // address. At most KW_MAX_LENGTH answer bytes, KW_MAX_PAYLOAD payload bytes.
  // Hardware-confirmed for a GFA read (VScotHO1_72, 2026-10-27).
  bool kwTelegram(uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr);
  static constexpr uint8_t KW_MAX_LENGTH = 64;
  static constexpr uint8_t KW_MAX_PAYLOAD = 32;
  // A read in the burner control's address space: kwTelegram() with 0x6B
  // (PacketVS1Type.GFA_READ).
  bool gfaRead(uint16_t address, uint8_t length);
  // How long the KW session waits for the controller's ENQ after the EOT that
  // drops P300 (the controller sends one about every 2 s when idle), and,
  // once an answer is in, for another KW telegram before returning to P300.
  // GFA_GAP_MS is the pause before a chained GFA read, as on KW
  // (VS1Engine::GFA_GAP_MS).
  static constexpr uint32_t KW_ENQ_TIMEOUT_MS = HANDSHAKE_RETRY_MS;
  static constexpr uint32_t KW_LINGER_MS = 250;
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
    // The KW detour of a KW telegram (kwTelegram()).
    KW_DROP,     // EOT: leave P300
    KW_ENQ,      // wait for the controller's ENQ
    KW_SEND,     // [01] <type> <addr hi> <addr lo> <len> [payload]
    KW_RECEIVE,  // <len> raw bytes, or the one acknowledgement byte
    KW_CHAIN,    // another KW telegram, or back to P300
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
  // A KW telegram in flight (kwTelegram()): its type, payload and frame,
  // with the ENQ_ACK in front when it opens the KW session, and its answer.
  bool _kw{false};
  uint8_t _kwType{0};
  bool _kwHasPayload{false};
  uint8_t _kwPayload[KW_MAX_PAYLOAD]{};
  uint8_t _kwFrame[5 + KW_MAX_PAYLOAD]{};
  uint8_t _kwFrameLen{0};
  uint8_t _kwAnswer[KW_MAX_LENGTH]{};
  // See errorPayload().
  uint8_t _errorPayload[ERROR_PAYLOAD_MAX]{};
  uint8_t _errorPayloadLength{0};
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
  void _kwDrop();
  void _kwEnq();
  void _kwSend();
  void _kwReceive();
  void _kwChain();
  void _kwStart(bool enqAck);

  void _tryOnResponse();
  void _tryOnError(OptolinkResult result);
};

}  // namespace esphome::vitohome::optolink
