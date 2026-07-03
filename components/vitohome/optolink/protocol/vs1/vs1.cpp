/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "vs1.h"

namespace esphome::vitohome::optolink {

VS1Engine::~VS1Engine() { delete _interface; }

void VS1Engine::onResponse(OnResponseCallback callback) { _onResponseCallback = callback; }
void VS1Engine::onError(OnErrorCallback callback) { _onErrorCallback = callback; }

bool VS1Engine::read(const Datapoint& datapoint) {
  if (_currentDatapoint) {
    return false;
  }
  if (datapoint.length() > kResponseBufferSize) {
    optolink_log_i("reading not possible, datapoint too large");
    return false;
  }
  if (_currentRequest.createPacket(PacketVS1Type.READ, datapoint.address(), datapoint.length())) {
    _currentDatapoint = datapoint;
    _requestTime = _currentMillis;
    optolink_log_i("reading packet OK");
    return true;
  }
  optolink_log_i("reading not possible, packet creation error");
  return false;
}

bool VS1Engine::write(const Datapoint& datapoint, const VariantValue& value) {
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

bool VS1Engine::write(const Datapoint& datapoint, const uint8_t* data, uint8_t length) {
  if (_currentDatapoint) {
    return false;
  }
  if (length != datapoint.length()) {
    optolink_log_i("writing not possible, length mismatch");
    return false;
  }
  if (datapoint.length() > kResponseBufferSize) {
    optolink_log_i("writing not possible, datapoint too large");
    return false;
  }
  if (_currentRequest.createPacket(PacketVS1Type.WRITE, datapoint.address(), datapoint.length(), data)) {
    _currentDatapoint = datapoint;
    _requestTime = _currentMillis;
    optolink_log_i("writing packet OK");
    return true;
  }
  optolink_log_i("writing not possible, packet creation error");
  return false;
}

bool VS1Engine::begin() {
  if (_interface->begin()) {
    while (_interface->available()) {
      _interface->read();  // clear rx buffer
    }
    _setState(State::INIT);
    return true;
  }
  return false;
}

void VS1Engine::loop() {
  _currentMillis = optolink_millis();
  switch (_state) {
    case State::INIT:
      _init();
      break;
    case State::SYNC_ENQ:
      _syncEnq();
      break;
    case State::SYNC_RECV:
      _syncRecv();
      break;
    case State::SEND:
      _send();
      break;
    case State::RECEIVE:
      _receive();
      break;
    case State::UNDEFINED:
      // begin() not yet called
      break;
  }
  // double timeout to accomodate for connection initialization
  if (_currentDatapoint && _currentMillis - _requestTime > REQUEST_TIMEOUT_MS) {
    _bytesTransferred = 0;
    _setState(State::INIT);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

void VS1Engine::end() {
  _interface->end();
  _setState(State::UNDEFINED);
  _currentDatapoint = Datapoint(nullptr, 0x0000, 0, noconv);
}

int VS1Engine::getState() const { return static_cast<std::underlying_type<State>::type>(_state); }

bool VS1Engine::isBusy() const {
  if (_currentDatapoint) {
    return true;
  }
  return false;
}

void VS1Engine::_setState(State state) {
  optolink_log_i("state %i --> %i", static_cast<std::underlying_type<State>::type>(_state),
                 static_cast<std::underlying_type<State>::type>(state));
  _state = state;
}

// wait for ENQ or reset connection if ENQ is not coming
void VS1Engine::_init() {
  if (_interface->available()) {
    if (_interface->read() == internals::ProtocolBytes.ENQ) {
      _lastMillis = _currentMillis;
      _setState(State::SYNC_ENQ);
    }
  } else {
    if (_currentMillis - _lastMillis > ENQ_RESET_INTERVAL_MS) {  // reset should Vitotronic be connected with VS2
      _lastMillis = _currentMillis;
      _interface->write(&internals::ProtocolBytes.EOT, 1);
    }
  }
}

// if we want to send something within SYNC_WINDOW_MS of receiving the ENQ, send ENQ_ACK and move to SEND
// if longer, return to INIT
void VS1Engine::_syncEnq() {
  if (_currentMillis - _lastMillis < SYNC_WINDOW_MS) {
    if (_currentDatapoint && _interface->write(&internals::ProtocolBytes.ENQ_ACK, 1) == 1) {
      _setState(State::SEND);
      _send();  // speed up things
    }
  } else {
    _setState(State::INIT);
  }
}

// if we want to send something within SYNC_WINDOW_MS of previous SEND, send again
// if longer, return to INIT
void VS1Engine::_syncRecv() {
  if (_currentMillis - _lastMillis < SYNC_WINDOW_MS) {
    if (_currentDatapoint) {
      _setState(State::SEND);
    }
  } else {
    _setState(State::INIT);
  }
}

// send request and move to RECEIVE
void VS1Engine::_send() {
  _bytesTransferred +=
      _interface->write(&_currentRequest[_bytesTransferred], _currentRequest.length() - _bytesTransferred);
  if (_bytesTransferred == _currentRequest.length()) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _setState(State::RECEIVE);
  }
}

// wait for data to receive
// when done, move to SYNC_RECV
void VS1Engine::_receive() {
  while (_interface->available()) {
    _responseBuffer[_bytesTransferred] = _interface->read();
    ++_bytesTransferred;
    _lastMillis = _currentMillis;
  }
  // Write-completion fix vs. upstream (THIRD_PARTY.md #11): upstream waited
  // for _currentDatapoint.length() response bytes after a WRITE too, but the
  // device acks a KW write (0xF4) with a SINGLE 0x00 byte -- hardware-
  // confirmed on a VScotHO1_72 (0x20CB): the 8-byte clock write got its 0x00
  // ack ~125 ms after the frame, then upstream's check waited for 8 bytes and
  // timed out. The coincidence len == 1 for the common 1-byte writes
  // (Betriebsart, setpoints) is what masked this. vcontrold's KW setaddr
  // ("RECV 1 SR") also reads exactly one byte and does not validate its
  // value; we complete on it and warn if it is not the documented 0x00.
  const uint8_t expected = (_currentRequest.packetType() == PacketVS1Type.WRITE) ? 1 : _currentDatapoint.length();
  if (_bytesTransferred == expected) {
    if (_currentRequest.packetType() == PacketVS1Type.WRITE && _responseBuffer[0] != 0x00) {
      optolink_log_w("write ack byte 0x%02x (expected 0x00)", static_cast<unsigned>(_responseBuffer[0]));
    }
    _bytesTransferred = 0;
    _setState(State::SYNC_RECV);
    _tryOnResponse(expected);
  }
}

void VS1Engine::_tryOnResponse(uint8_t length) {
  if (_onResponseCallback) {
    _onResponseCallback(_responseBuffer.data(), length, _currentDatapoint);
  }
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

void VS1Engine::_tryOnError(OptolinkResult result) {
  if (_onErrorCallback) {
    _onErrorCallback(result, _currentDatapoint);
  }
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

}  // namespace esphome::vitohome::optolink
