/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "gwg.h"

namespace esphome {
namespace vitohome {
namespace optolink {

GWGEngine::~GWGEngine() { delete _interface; }

void GWGEngine::onResponse(OnResponseCallback callback) { _onResponseCallback = callback; }
void GWGEngine::onError(OnErrorCallback callback) { _onErrorCallback = callback; }

bool GWGEngine::read(const Datapoint &datapoint) {
  if (_currentDatapoint) {
    return false;
  }
  if (datapoint.length() > kResponseBufferSize) {
    optolink_log_i("reading not possible, datapoint too large");
    return false;
  }
  if (_currentRequest.createPacket(PacketGWGType.READ, datapoint.address(), datapoint.length())) {
    _currentDatapoint = datapoint;
    _requestTime = _currentMillis;
    optolink_log_i("reading packet OK");
    return true;
  }
  optolink_log_i("reading not possible, packet creation error");
  return false;
}

bool GWGEngine::write(const Datapoint &datapoint, const VariantValue &value) {
  if (_currentDatapoint) {
    return false;
  }
  uint8_t *payload = reinterpret_cast<uint8_t *>(malloc(datapoint.length()));
  if (!payload) return false;
  datapoint.encode(payload, datapoint.length(), value);
  bool result = write(datapoint, payload, datapoint.length());
  free(payload);
  return result;
}

bool GWGEngine::write(const Datapoint &datapoint, const uint8_t *data, uint8_t length) {
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
  if (_currentRequest.createPacket(PacketGWGType.WRITE, datapoint.address(), datapoint.length(), data)) {
    _currentDatapoint = datapoint;
    _requestTime = _currentMillis;
    optolink_log_i("writing packet OK");
    return true;
  }
  optolink_log_i("writing not possible, packet creation error");
  return false;
}

bool GWGEngine::begin() {
  _setState(State::INIT);
  return _interface->begin();
}

void GWGEngine::loop() {
  _currentMillis = optolink_millis();
  switch (_state) {
    case State::INIT:
      _init();
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
    _setState(State::INIT);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

void GWGEngine::end() {
  _interface->end();
  _setState(State::UNDEFINED);
  _currentDatapoint = Datapoint(nullptr, 0x0000, 0, noconv);
}

int GWGEngine::getState() const { return static_cast<std::underlying_type<State>::type>(_state); }

bool GWGEngine::isBusy() const {
  if (_currentDatapoint) {
    return true;
  }
  return false;
}

void GWGEngine::_setState(State state) {
  optolink_log_i("state %i --> %i", static_cast<std::underlying_type<State>::type>(_state),
                 static_cast<std::underlying_type<State>::type>(state));
  _state = state;
}

void GWGEngine::_init() {
  if (_interface->available()) {
    if (_interface->read() == internals::ProtocolBytes.ENQ && _currentDatapoint) {
      _bytesTransferred = 0;
      _setState(State::SEND);
    }
  }
}

void GWGEngine::_send() {
  _bytesTransferred +=
      _interface->write(&_currentRequest[_bytesTransferred], _currentRequest.length() - _bytesTransferred);
  if (_bytesTransferred == _currentRequest.length()) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _setState(State::RECEIVE);
  }
}

void GWGEngine::_receive() {
  while (_interface->available()) {
    _responseBuffer[_bytesTransferred] = _interface->read();
    ++_bytesTransferred;
    _lastMillis = _currentMillis;
  }
  if (_bytesTransferred == _currentRequest.length()) {
    _setState(State::INIT);
    _tryOnResponse();
  }
}

void GWGEngine::_tryOnResponse() {
  if (_onResponseCallback) {
    _onResponseCallback(_responseBuffer.data(), _currentRequest.length(), _currentDatapoint);
  }
  // Bugfix vs. upstream: clear the current datapoint after a successful
  // response, matching VS1/VS2. Without this, GWG refuses every read/write
  // after the first success (one-shot). See THIRD_PARTY.md.
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

void GWGEngine::_tryOnError(OptolinkResult result) {
  if (_onErrorCallback) {
    _onErrorCallback(result, _currentDatapoint);
  }
  _currentDatapoint = Datapoint(nullptr, 0, 0, noconv);
}

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
