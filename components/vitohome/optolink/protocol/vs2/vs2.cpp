/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "vs2.h"

#include <cstring>

namespace esphome::vitohome::optolink {

VS2Engine::~VS2Engine() { delete _interface; }

void VS2Engine::onResponse(OnResponseCallback callback) { _onResponseCallback = callback; }
void VS2Engine::onError(OnErrorCallback callback) { _onErrorCallback = callback; }

bool VS2Engine::read(uint16_t address, uint8_t length) {
  return request(static_cast<uint8_t>(FunctionCode::READ), address, length);
}

bool VS2Engine::write(uint16_t address, const uint8_t *data, uint8_t length) {
  if (data == nullptr)
    return false;
  return request(static_cast<uint8_t>(FunctionCode::WRITE), address, length, data);
}

bool VS2Engine::rpc(uint16_t address, const uint8_t *parameter, uint8_t length) {
  if (length != 0 && parameter == nullptr)
    return false;
  return request(static_cast<uint8_t>(FunctionCode::RPC), address, length, length != 0 ? parameter : nullptr);
}

bool VS2Engine::request(uint8_t function, uint16_t address, uint8_t length, const uint8_t *data) {
  if (_busy) {
    return false;
  }
  // The flags byte holds the function code in its low five bits; the upper
  // three are the message id (always 0 here).
  if (function == 0 || function > 0x1F) {
    optolink_log_i("function code 0x%02x not possible (1..0x1f)", function);
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, static_cast<FunctionCode>(function), 0, address, length, data)) {
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    _kw = false;
    _errorPayloadLength = 0;
    _parser.expectFunction(function);
    optolink_log_i("function 0x%02x packet OK", function);
    return true;
  }
  optolink_log_i("function 0x%02x not possible, packet creation error", function);
  return false;
}

bool VS2Engine::gfaRead(uint16_t address, uint8_t length) {
  return kwTelegram(PacketVS1Type.GFA_READ, address, length);
}

bool VS2Engine::kwTelegram(uint8_t type, uint16_t address, uint8_t length, const uint8_t *data) {
  if (_busy) {
    return false;
  }
  if (length == 0 || length > (data ? KW_MAX_PAYLOAD : KW_MAX_LENGTH)) {
    optolink_log_i("kw telegram not possible, length %u out of 1..%u", length, data ? KW_MAX_PAYLOAD : KW_MAX_LENGTH);
    return false;
  }
  _kw = true;
  _kwType = type;
  _kwHasPayload = data != nullptr;
  if (data)
    std::memcpy(_kwPayload, data, length);
  _currentAddress = address;
  _currentLength = length;
  _errorPayloadLength = 0;
  _busy = true;
  _requestTime = _currentMillis;
  optolink_log_i("kw telegram 0x%02x OK", type);
  return true;
}

const uint8_t *VS2Engine::errorPayload(uint8_t &length) const {
  length = _errorPayloadLength;
  return _errorPayload;
}

bool VS2Engine::begin() {
  if (_interface == nullptr)
    return false;
  _setState(State::RESET);
  return _interface->begin();
}

void VS2Engine::loop() {
  _currentMillis = optolink_millis();
  switch (_state) {
    case State::RESET:
      _reset();
      break;
    case State::RESET_ACK:
      _resetAck();
      break;
    case State::INIT:
      _init();
      break;
    case State::INIT_ACK:
      _initAck();
      break;
    case State::IDLE:
      _idle();
      break;
    case State::SENDSTART:
      _sendStart();
      break;
    case State::SENDPACKET:
      _sendPacket();
      break;
    case State::SEND_CRC:
      _sendCRC();
      break;
    case State::SEND_ACK:
      _sendAck();
      break;
    case State::RECEIVE:
      _receive();
      break;
    case State::RECEIVE_ACK:
      _receiveAck();
      break;
    case State::KW_DROP:
      _kwDrop();
      break;
    case State::KW_ENQ:
      _kwEnq();
      break;
    case State::KW_SEND:
      _kwSend();
      break;
    case State::KW_RECEIVE:
      _kwReceive();
      break;
    case State::KW_CHAIN:
      _kwChain();
      break;
    case State::UNDEFINED:
      // begin() not yet called
      break;
  }
  if (_busy && _currentMillis - _requestTime > REQUEST_TIMEOUT_MS) {
    _setState(State::RESET);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

void VS2Engine::end() {
  _interface->end();
  _setState(State::UNDEFINED);
  _busy = false;
}

bool VS2Engine::isBusy() const { return _busy; }

void VS2Engine::_setState(State state) {
  optolink_log_i("state %i --> %i", static_cast<std::underlying_type<State>::type>(_state),
                 static_cast<std::underlying_type<State>::type>(state));
  _state = state;
  // _bytesTransferred counts within one state (SYNC sent, packet sent, KW
  // telegram sent, KW answer received); every state that counts starts at 0.
  // Before this, the request watchdog could leave a partial KW answer's count
  // behind on its way to RESET, and _init() then sent SYNC from past the end
  // of the 3-byte array with a wrapped length (found by fuzz_raw_engines).
  _bytesTransferred = 0;
}

void VS2Engine::_reset() {
  // Parser-state fix vs. upstream (THIRD_PARTY.md #10): a request that times
  // out mid-frame used to leave the byte-at-a-time parser stuck mid-PAYLOAD,
  // so the next transaction's frame was consumed as payload continuation and
  // failed with CS_ERROR before self-healing. Every path into RESET now also
  // resets the parser, matching the RX-buffer drain below.
  _parser.reset();
  while (_interface->available())
    _interface->read();
  if (_interface->write(&internals::ProtocolBytes.EOT, 1) == 1) {
    _lastMillis = _currentMillis;
    _setState(State::RESET_ACK);
  }
}

void VS2Engine::_resetAck() {
  if (_interface->available()) {
    uint8_t buff = _interface->read();
    if (buff == internals::ProtocolBytes.ENQ) {
      _lastMillis = _currentMillis;
      _setState(State::INIT);
    }
  } else {
    if (_currentMillis - _lastMillis > HANDSHAKE_RETRY_MS) {
      _setState(State::RESET);
    }
  }
}

void VS2Engine::_init() {
  _bytesTransferred += _interface->write(&internals::ProtocolBytes.SYNC[_bytesTransferred],
                                         sizeof(internals::ProtocolBytes.SYNC) - _bytesTransferred);
  if (_bytesTransferred == sizeof(internals::ProtocolBytes.SYNC)) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _setState(State::INIT_ACK);
  }
}

void VS2Engine::_initAck() {
  if (_interface->available()) {
    uint8_t buff = _interface->read();
    optolink_log_i("rcv: 0x%02x", buff);
    if (buff == internals::ProtocolBytes.ACK) {
      _setState(State::IDLE);
    } else {
      _setState(State::RESET);
    }
  } else if (_currentMillis - _lastMillis > HANDSHAKE_RETRY_MS) {
    _setState(State::RESET);
  }
}

void VS2Engine::_idle() {
  if (_busy) {
    // A KW telegram leaves P300 for KW (see kwTelegram()).
    if (_kw) {
      _setState(State::KW_DROP);
      return;
    }
    _setState(State::SENDSTART);
  }
  // send INIT every KEEPALIVE_INTERVAL_MS to keep communication alive
  if (_currentMillis - _lastMillis > KEEPALIVE_INTERVAL_MS) {
    _setState(State::INIT);
  }
}

void VS2Engine::_sendStart() {
  if (_interface->write(&internals::ProtocolBytes.PACKETSTART, 1) == 1) {
    _lastMillis = _currentMillis;
    _setState(State::SENDPACKET);
  }
}

void VS2Engine::_sendPacket() {
  _bytesTransferred +=
      _interface->write(&_currentPacket[_bytesTransferred], _currentPacket.length() - _bytesTransferred);
  if (_bytesTransferred == _currentPacket.length()) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _setState(State::SEND_CRC);
  }
}

void VS2Engine::_sendCRC() {
  uint8_t crc = _currentPacket.checksum();
  if (_interface->write(&crc, 1) == 1) {
    _lastMillis = _currentMillis;
    _setState(State::SEND_ACK);
  }
}

void VS2Engine::_sendAck() {
  if (_interface->available()) {
    uint8_t buff = _interface->read();
    optolink_log_i("rcv: 0x%02x", buff);
    if (buff == internals::ProtocolBytes.ACK) {  // transmit succesful, moving to next state
      _setState(State::RECEIVE);
    } else if (buff == internals::ProtocolBytes.NACK) {  // transmit negatively acknowledged, return to IDLE
      _setState(State::IDLE);
      _tryOnError(OptolinkResult::NACK);
      return;
    }
  }
}

void VS2Engine::_receive() {
  while (_interface->available()) {
    _lastMillis = _currentMillis;
    internals::ParserResult result = _parser.parse(_interface->read());
    if (result == internals::ParserResult::COMPLETE) {
      // Frame-type guard vs. upstream (THIRD_PARTY.md #9): upstream delivered
      // ANY complete frame -- including a device ERROR frame (PacketType 0x03)
      // -- through the response callback, so an error frame's payload was
      // decoded and published as data. The link-layer choreography is
      // unchanged (the frame is still ACKed via RECEIVE_ACK); only a
      // non-RESPONSE type is now routed to the error callback instead, as
      // DEVICE_ERROR: the frame is complete and checksum-valid, so the peer
      // demonstrably answered -- distinct from the malformed-traffic ERROR
      // below, which proves nothing about the link (see constants.h).
      _setState(State::RECEIVE_ACK);
      if (_parser.packet().packetType() == PacketType::RESPONSE &&
          _parser.packet().functionCode() != _currentPacket.functionCode()) {
        // An answer to another function code -- a write's acknowledgement
        // arriving for a read, a stale frame -- is not this request's answer
        // (upstream delivered it, as a read with no data and the frame's
        // DataLength; found by fuzz_raw_engines). Malformed for this request,
        // though a valid frame, so it is ACKed and reported as ERROR.
        optolink_log_w("response function code 0x%02x does not match the request's 0x%02x",
                       static_cast<unsigned>(_parser.packet().functionCode()),
                       static_cast<unsigned>(_currentPacket.functionCode()));
        _tryOnError(OptolinkResult::ERROR);
      } else if (_parser.packet().packetType() == PacketType::RESPONSE) {
        _tryOnResponse();
      } else {
        optolink_log_w("packet type 0x%02x is not a response", static_cast<unsigned>(_parser.packet().packetType()));
        // Keep the frame's payload -- the controller's error code -- for
        // errorPayload().
        const PacketVS2 &packet = _parser.packet();
        _errorPayloadLength = packet.payloadLength() < ERROR_PAYLOAD_MAX ? packet.payloadLength() : ERROR_PAYLOAD_MAX;
        std::memcpy(_errorPayload, packet.payload(), _errorPayloadLength);
        _tryOnError(OptolinkResult::DEVICE_ERROR);
      }
      return;
    } else if (result == internals::ParserResult::CS_ERROR) {
      _setState(State::RESET);
      _tryOnError(OptolinkResult::CRC);
      return;
    } else if (result == internals::ParserResult::ERROR) {
      _setState(State::RESET);
      _tryOnError(OptolinkResult::ERROR);
      return;
    }
    // else: continue
  }
}

void VS2Engine::_receiveAck() {
  if (_interface->write(&internals::ProtocolBytes.ACK, 1) == 1) {
    _lastMillis = _currentMillis;
    _setState(State::IDLE);
  }
}

// --- A KW telegram: the detour (kwTelegram()) ---------------------------------
// The controller answers the KW telegram 0x6B (hardware-confirmed over KW) but
// refuses it as a P300 function code (an ERROR frame, code 05). So the link
// leaves P300 the way RESET does, with an EOT, and the controller -- now in KW
// -- offers its ENQ; the telegram goes out on it like a KW read. Further KW
// telegrams chain onto that session; then RESET re-establishes P300.
// Hardware-confirmed for a GFA read on VScotHO1_72 (2026-10-27).

// Leave P300 like _reset(): parser, stale bytes, EOT.
void VS2Engine::_kwDrop() {
  _parser.reset();
  while (_interface->available())
    _interface->read();
  if (_interface->write(&internals::ProtocolBytes.EOT, 1) == 1) {
    _lastMillis = _currentMillis;
    // The request watchdog runs from here: the request may have waited out a
    // handshake before the link was idle.
    _requestTime = _currentMillis;
    _setState(State::KW_ENQ);
  }
}

void VS2Engine::_kwEnq() {
  if (_interface->available()) {
    // Anything but the ENQ is left over from P300 (the controller ACKs the
    // EOT first) and dropped.
    if (_interface->read() == internals::ProtocolBytes.ENQ)
      _kwStart(true);
  } else if (_currentMillis - _lastMillis > KW_ENQ_TIMEOUT_MS) {
    _setState(State::RESET);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

// The KW telegram, with ENQ_ACK in front when it answers an ENQ (as
// VS1Engine::_syncEnq() does), sent in the same loop as the ENQ arrived.
void VS2Engine::_kwStart(bool enqAck) {
  uint8_t n = 0;
  if (enqAck)
    _kwFrame[n++] = internals::ProtocolBytes.ENQ_ACK;
  _kwFrame[n++] = _kwType;
  _kwFrame[n++] = static_cast<uint8_t>(_currentAddress >> 8);
  _kwFrame[n++] = static_cast<uint8_t>(_currentAddress & 0xFF);
  _kwFrame[n++] = _currentLength;
  if (_kwHasPayload) {
    std::memcpy(&_kwFrame[n], _kwPayload, _currentLength);
    n += _currentLength;
  }
  _kwFrameLen = n;
  _bytesTransferred = 0;
  _requestTime = _currentMillis;
  _setState(State::KW_SEND);
  _kwSend();
}

void VS2Engine::_kwSend() {
  _bytesTransferred += _interface->write(&_kwFrame[_bytesTransferred], _kwFrameLen - _bytesTransferred);
  if (_bytesTransferred == _kwFrameLen) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _setState(State::KW_RECEIVE);
  }
}

// KW answers with exactly <length> raw bytes, or one acknowledgement byte
// after a telegram with payload: no frame, no checksum.
void VS2Engine::_kwReceive() {
  const uint8_t expected = _kwHasPayload ? 1 : _currentLength;
  while (_interface->available() && _bytesTransferred < expected) {
    _kwAnswer[_bytesTransferred++] = _interface->read();
    _lastMillis = _currentMillis;
  }
  if (_bytesTransferred < expected)
    return;
  _bytesTransferred = 0;
  _setState(State::KW_CHAIN);
  if (_onResponseCallback) {
    _onResponseCallback(_kwAnswer, expected, _currentAddress);
  }
  _busy = false;
}

// After an answer the KW session is still open: another KW telegram goes out
// on it (a GFA read GFA_GAP_MS after the answer); anything else, or
// KW_LINGER_MS without a request, returns to P300. A byte from the controller
// meanwhile is its ENQ (the session idled out): it opens the next KW telegram,
// or is answered with SYNC like the ENQ RESET waits for.
void VS2Engine::_kwChain() {
  if (_interface->available()) {
    if (_interface->read() != internals::ProtocolBytes.ENQ) {
      _setState(State::RESET);
      return;
    }
    if (_busy && _kw) {
      _kwStart(true);
    } else {
      _lastMillis = _currentMillis;
      _setState(State::INIT);
    }
    return;
  }
  if (_busy && _kw) {
    if (_kwType != PacketVS1Type.GFA_READ || _currentMillis - _lastMillis >= GFA_GAP_MS)
      _kwStart(false);
    return;
  }
  if (_busy || _currentMillis - _lastMillis > KW_LINGER_MS) {
    _setState(State::RESET);
  }
}

void VS2Engine::_tryOnResponse() {
  if (_onResponseCallback) {
    // Surface the payload and the address ECHOED in the response frame (not
    // the request's) so the caller's response-address match is a real
    // wire-level check on P300. A write ack has data()==nullptr by design but
    // a non-zero dataLength(); the caller must guard on data() before reading.
    // An RPC answer is as long as its frame says (see ParserVS2), and so is
    // the answer to a function code without a frame shape of its own
    // (request()).
    const PacketVS2 &packet = _parser.packet();
    const FunctionCode fc = packet.functionCode();
    const bool sized_by_frame = fc != FunctionCode::READ && fc != FunctionCode::WRITE;
    const uint8_t length = sized_by_frame ? packet.payloadLength() : packet.dataLength();
    _onResponseCallback(packet.data(), length, packet.address());
  }
  _busy = false;
}

void VS2Engine::_tryOnError(OptolinkResult result) {
  if (_onErrorCallback) {
    _onErrorCallback(result, _currentAddress);
  }
  _busy = false;
}

}  // namespace esphome::vitohome::optolink
