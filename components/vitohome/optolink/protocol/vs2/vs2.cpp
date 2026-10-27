/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "vs2.h"

namespace esphome::vitohome::optolink {

VS2Engine::~VS2Engine() { delete _interface; }

void VS2Engine::onResponse(OnResponseCallback callback) { _onResponseCallback = callback; }
void VS2Engine::onError(OnErrorCallback callback) { _onErrorCallback = callback; }

bool VS2Engine::read(uint16_t address, uint8_t length) {
  if (_busy) {
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, FunctionCode::READ, 0, address, length)) {
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    _gfa = false;
    _probeFlags = 0;
    _parser.expectFlags(0);
    optolink_log_i("reading packet OK");
    return true;
  }
  optolink_log_i("reading not possible, packet creation error");
  return false;
}

bool VS2Engine::gfaRead(uint16_t address, uint8_t length) {
  if (_busy) {
    return false;
  }
  if (length == 0 || length > GFA_MAX_LENGTH) {
    optolink_log_i("gfa reading not possible, length %u out of 1..%u", length, GFA_MAX_LENGTH);
    return false;
  }
  _gfa = true;
  _currentAddress = address;
  _currentLength = length;
  _busy = true;
  _requestTime = _currentMillis;
  _probeFlags = 0;
  _parser.expectFlags(0);
  optolink_log_i("gfa reading OK");
  return true;
}

bool VS2Engine::functionRead(uint8_t functionByte, uint16_t address, uint8_t length) {
  if (_busy || functionByte == 0) {
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, FunctionCode::READ, 0, address, length)) {
    _currentPacket[2] = functionByte;  // the whole flags byte; the checksum is taken at send
    _gfa = false;
    _probeFlags = functionByte;
    _parser.expectFlags(functionByte);
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    optolink_log_i("function 0x%02x probe packet OK", functionByte);
    return true;
  }
  optolink_log_i("function probe not possible, packet creation error");
  return false;
}

bool VS2Engine::write(uint16_t address, const uint8_t *data, uint8_t length) {
  if (_busy) {
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, FunctionCode::WRITE, 0, address, length, data)) {
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    _gfa = false;
    _probeFlags = 0;
    _parser.expectFlags(0);
    optolink_log_i("writing packet OK");
    return true;
  }
  optolink_log_i("writing not possible, packet creation error");
  return false;
}

bool VS2Engine::rpc(uint16_t address, const uint8_t *parameter, uint8_t length) {
  if (_busy) {
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, FunctionCode::RPC, 0, address, length, parameter)) {
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    _gfa = false;
    _probeFlags = 0;
    _parser.expectFlags(0);
    optolink_log_i("rpc packet OK");
    return true;
  }
  optolink_log_i("rpc not possible, packet creation error");
  return false;
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
    case State::GFA_DROP:
      _gfaDrop();
      break;
    case State::GFA_ENQ:
      _gfaEnq();
      break;
    case State::GFA_SEND:
      _gfaSend();
      break;
    case State::GFA_RECEIVE:
      _gfaReceive();
      break;
    case State::GFA_CHAIN:
      _gfaChain();
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
    // A GFA read leaves P300 for one KW telegram (see gfaRead()).
    if (_gfa) {
      _setState(State::GFA_DROP);
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
      if (_parser.packet().packetType() == PacketType::RESPONSE) {
        _tryOnResponse();
      } else {
        optolink_log_w("packet type 0x%02x is not a response", static_cast<unsigned>(_parser.packet().packetType()));
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

// --- GFA read: a KW detour (gfaRead()) ---------------------------------------
// The controller answers the KW telegram 0x6B (hardware-confirmed over KW) but
// refuses it as a P300 function code (an ERROR frame, code 05). So the link
// leaves P300 the way RESET does, with an EOT, and the controller -- now in KW
// -- offers its ENQ; the telegram goes out on it like a KW read. Further GFA
// reads chain onto that session; then RESET re-establishes P300.

// Leave P300 like _reset(): parser, stale bytes, EOT.
void VS2Engine::_gfaDrop() {
  _parser.reset();
  while (_interface->available())
    _interface->read();
  if (_interface->write(&internals::ProtocolBytes.EOT, 1) == 1) {
    _lastMillis = _currentMillis;
    // The request watchdog runs from here: the request may have waited out a
    // handshake before the link was idle.
    _requestTime = _currentMillis;
    _setState(State::GFA_ENQ);
  }
}

void VS2Engine::_gfaEnq() {
  if (_interface->available()) {
    // Anything but the ENQ is left over from P300 and dropped.
    if (_interface->read() == internals::ProtocolBytes.ENQ)
      _gfaStart(true);
  } else if (_currentMillis - _lastMillis > GFA_ENQ_TIMEOUT_MS) {
    _setState(State::RESET);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

// The KW telegram, with ENQ_ACK in front when it answers an ENQ (as
// VS1Engine::_syncEnq() does), sent in the same loop as the ENQ arrived.
void VS2Engine::_gfaStart(bool enqAck) {
  uint8_t n = 0;
  if (enqAck)
    _gfaFrame[n++] = internals::ProtocolBytes.ENQ_ACK;
  _gfaFrame[n++] = PacketVS1Type.GFA_READ;
  _gfaFrame[n++] = static_cast<uint8_t>(_currentAddress >> 8);
  _gfaFrame[n++] = static_cast<uint8_t>(_currentAddress & 0xFF);
  _gfaFrame[n++] = _currentLength;
  _gfaFrameLen = n;
  _bytesTransferred = 0;
  _requestTime = _currentMillis;
  _setState(State::GFA_SEND);
  _gfaSend();
}

void VS2Engine::_gfaSend() {
  _bytesTransferred += _interface->write(&_gfaFrame[_bytesTransferred], _gfaFrameLen - _bytesTransferred);
  if (_bytesTransferred == _gfaFrameLen) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _setState(State::GFA_RECEIVE);
  }
}

// KW answers with exactly <length> raw bytes: no frame, no checksum.
void VS2Engine::_gfaReceive() {
  while (_interface->available() && _bytesTransferred < _currentLength) {
    _gfaAnswer[_bytesTransferred++] = _interface->read();
    _lastMillis = _currentMillis;
  }
  if (_bytesTransferred < _currentLength)
    return;
  _bytesTransferred = 0;
  _setState(State::GFA_CHAIN);
  if (_onResponseCallback) {
    _onResponseCallback(_gfaAnswer, _currentLength, _currentAddress);
  }
  _busy = false;
}

// After an answer the KW session is still open: a GFA read goes out on it after
// GFA_GAP_MS; anything else, or GFA_LINGER_MS without a request, returns to
// P300. A byte from the controller meanwhile is its ENQ (the session idled
// out): it opens the next GFA telegram, or is answered with SYNC like the ENQ
// RESET waits for.
void VS2Engine::_gfaChain() {
  if (_interface->available()) {
    if (_interface->read() != internals::ProtocolBytes.ENQ) {
      _setState(State::RESET);
      return;
    }
    if (_busy && _gfa) {
      _gfaStart(true);
    } else {
      _lastMillis = _currentMillis;
      _setState(State::INIT);
    }
    return;
  }
  if (_busy && _gfa) {
    if (_currentMillis - _lastMillis >= GFA_GAP_MS)
      _gfaStart(false);
    return;
  }
  if (_busy || _currentMillis - _lastMillis > GFA_LINGER_MS) {
    _setState(State::RESET);
  }
}

void VS2Engine::_tryOnResponse() {
  if (_onResponseCallback) {
    // Surface the payload and the address ECHOED in the response frame (not
    // the request's) so the caller's response-address match is a real
    // wire-level check on P300. A write ack has data()==nullptr by design but
    // a non-zero dataLength(); the caller must guard on data() before reading.
    // An RPC answer is as long as its frame says (see ParserVS2).
    const PacketVS2 &packet = _parser.packet();
    if (_probeFlags != 0) {
      // A probe's answer is read-shaped whatever its flags byte's low bits say.
      _onResponseCallback(packet.payload(), packet.dataLength(), packet.address());
    } else {
      const uint8_t length = packet.functionCode() == FunctionCode::RPC ? packet.payloadLength() : packet.dataLength();
      _onResponseCallback(packet.data(), length, packet.address());
    }
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
