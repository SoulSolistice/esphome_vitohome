/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "vs2.h"

namespace esphome {
namespace vitohome {
namespace optolink {

VS2Engine::~VS2Engine() { delete _interface; }

void VS2Engine::onResponse(OnResponseCallback callback) { _onResponseCallback = callback; }
void VS2Engine::onError(OnErrorCallback callback) { _onErrorCallback = callback; }

bool VS2Engine::read(const Datapoint& datapoint) {
  if (_currentDatapoint) {
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, FunctionCode::READ, 0, datapoint.address(),
                                  datapoint.length())) {
    _currentDatapoint = datapoint;
    _requestTime = _currentMillis;
    optolink_log_i("reading packet OK");
    return true;
  }
  optolink_log_i("reading not possible, packet creation error");
  return false;
}

bool VS2Engine::write(const Datapoint& datapoint, const VariantValue& value) {
  if (_currentDatapoint) {
    return false;
  }
  uint8_t* payload = reinterpret_cast<uint8_t*>(malloc(datapoint.length()));
  if (!payload) return false;
  datapoint.encode(payload, datapoint.length(), value);
  bool result = write(datapoint, payload, datapoint.length());
  free(payload);
  return result;
}

bool VS2Engine::write(const Datapoint& datapoint, const uint8_t* data, uint8_t length) {
  if (_currentDatapoint) {
    return false;
  }
  if (length != datapoint.length()) {
    optolink_log_i("writing not possible, length error");
    return false;
  }
  if (_currentPacket.createPacket(PacketType::REQUEST, FunctionCode::WRITE, 0, datapoint.address(), datapoint.length(),
                                  data)) {
    _currentDatapoint = datapoint;
    _requestTime = _currentMillis;
    optolink_log_i("writing packet OK");
    return true;
  }
  optolink_log_i("writing not possible, packet creation error");
  return false;
}

bool VS2Engine::begin() {
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
    case State::UNDEFINED:
      // begin() not yet called
      break;
  }
  if (_currentDatapoint && _currentMillis - _requestTime > REQUEST_TIMEOUT_MS) {
    _setState(State::RESET);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

void VS2Engine::end() {
  _interface->end();
  _setState(State::UNDEFINED);
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

int VS2Engine::getState() const { return static_cast<std::underlying_type<State>::type>(_state); }

bool VS2Engine::isBusy() const {
  if (_currentDatapoint) {
    return true;
  }
  return false;
}

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
  while (_interface->available()) _interface->read();
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
  if (_currentDatapoint) {
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
      // non-RESPONSE type is now routed to the error callback instead.
      _setState(State::RECEIVE_ACK);
      if (_parser.packet().packetType() == PacketType::RESPONSE) {
        _tryOnResponse();
      } else {
        optolink_log_w("packet type 0x%02x is not a response", static_cast<unsigned>(_parser.packet().packetType()));
        _tryOnError(OptolinkResult::ERROR);
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

void VS2Engine::_tryOnResponse() {
  if (_onResponseCallback) {
    _onResponseCallback(_parser.packet(), _currentDatapoint);
  }
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

void VS2Engine::_tryOnError(OptolinkResult result) {
  if (_onErrorCallback) {
    _onErrorCallback(result, _currentDatapoint);
  }
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
