/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "gwg.h"

namespace esphome::vitohome::optolink {

GWGEngine::~GWGEngine() { delete _interface; }

void GWGEngine::onResponse(OnResponseCallback callback) { _onResponseCallback = callback; }
void GWGEngine::onError(OnErrorCallback callback) { _onErrorCallback = callback; }
void GWGEngine::onTiming(OnTimingCallback callback) { _onTimingCallback = callback; }

bool GWGEngine::read(uint16_t address, uint8_t length, GWGAccessMode access) {
  if (_busy) {
    return false;
  }
  if (_currentRequest.createPacket(gwgReadTypeByte(access), address, length)) {
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    optolink_log_i("reading packet OK");
    return true;
  }
  optolink_log_i("reading not possible, packet creation error");
  return false;
}

bool GWGEngine::write(uint16_t address, const uint8_t *data, uint8_t length, GWGAccessMode access) {
  if (_busy) {
    return false;
  }
  // The KMBUS modes have no write TYPE byte in the wiki table, so a write in
  // one of them is not expressible rather than merely unverified. Refuse it
  // here (permanent rejection, same shape as an out-of-range address) instead
  // of silently falling back to a different mode.
  if (!gwgModeIsWritable(access)) {
    optolink_log_w("GWG access mode has no write telegram type");
    return false;
  }
  if (_currentRequest.createPacket(gwgWriteTypeByte(access), address, length, data)) {
    _currentAddress = address;
    _currentLength = length;
    _busy = true;
    _requestTime = _currentMillis;
    optolink_log_i("writing packet OK");
    return true;
  }
  optolink_log_i("writing not possible, packet creation error");
  return false;
}

bool GWGEngine::begin() {
  if (_interface == nullptr)
    return false;
  _setState(State::INIT);
  return _interface->begin();
}

void GWGEngine::loop() {
  // Host-stall measure, feeding the timing instrument (see OnTimingCallback).
  // The device's ENQ is read out of a buffer, so its true arrival time is not
  // observable; the gap since the previous iteration is an upper bound on how
  // stale an ENQ consumed this iteration can be, and it is the quantity that
  // actually decides whether the KW-family sync window is being kept.
  const uint32_t previousLoopMillis = _currentMillis;
  _currentMillis = optolink_millis();
  _loopGapMillis = _currentMillis - previousLoopMillis;
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
  if (_busy && _currentMillis - _requestTime > REQUEST_TIMEOUT_MS) {
    _setState(State::INIT);
    _tryOnError(OptolinkResult::TIMEOUT);
  }
}

void GWGEngine::end() {
  _interface->end();
  _setState(State::UNDEFINED);
  _busy = false;
}

bool GWGEngine::isBusy() const { return _busy; }

void GWGEngine::_setState(State state) {
  optolink_log_i("state %i --> %i", static_cast<std::underlying_type<State>::type>(_state),
                 static_cast<std::underlying_type<State>::type>(state));
  _state = state;
}

void GWGEngine::_init() {
  if (_interface->available()) {
    if (_interface->read() == internals::ProtocolBytes.ENQ && _busy) {
      _bytesTransferred = 0;
      _enqAgeMillis = _loopGapMillis;    // timing instrument (see OnTimingCallback)
      _lastSyncMillis = _currentMillis;  // burst window opens (see ENQ_VALIDITY_MS)
      _syncValid = true;
      _sentInBurst = false;
      _setState(State::SEND);
      // Fall through into _send() rather than waiting for the next loop()
      // iteration (divergence from upstream). The KW-family master has to
      // answer the device's ENQ inside a short window; deferring the write by a
      // whole component loop spends that budget on unrelated work. On the
      // hardware capture that motivated RESPONSE_TIMEOUT_MS the unanswered
      // requests clustered at the start of the poll cycle, where ESPHome's 60 s
      // entity publishes and the API state flush lengthen the loop - exactly
      // when this extra iteration costs the most. _send() is idempotent with
      // respect to state (it advances to RECEIVE only once the whole frame is
      // out), so calling it here is equivalent to the deferred call, minus the
      // latency. Cross-validated against dannerph/esphome_vitoconnect, whose
      // _idle() does the same thing with the same rationale stated explicitly.
      _send();
      return;
    }
    // A byte was consumed but it was not a usable ENQ (line noise, or a
    // straggler while idle). Stop here rather than falling through: more bytes
    // may still be buffered, and the burst path below must only ever run on a
    // genuinely empty interface.
    return;
  }
  // Burst (see ENQ_VALIDITY_MS): nothing is buffered, but a sync we already
  // earned is still inside its window, so send without waiting for the next
  // ENQ. Reached only when the interface has NO bytes pending -- a real ENQ
  // always wins, and no buffered byte can be mistaken for the response, so
  // this needs no drain. It is also what bounds the window in practice: it can
  // only be ridden while the device has not spoken since the last answer.
  if (_busy && _canBurst()) {
    _bytesTransferred = 0;
    _enqAgeMillis = _currentMillis - _lastSyncMillis;  // age of the sync we are riding
    _sentInBurst = true;
    _setState(State::SEND);
    _send();
    return;
  }
  if constexpr (SEND_ENQ_POKE) {
    // Active sync (opt-in, see SEND_ENQ_POKE): nudge the device with an EOT
    // (0x04) while waiting for its ENQ (0x05). Mirrors vcontrold's GWG sync
    // (SEND 04; WAIT 05) and the VS1 EOT fallback. The default build discards
    // this branch, so passive-wait behaviour is unchanged.
    if (_busy && (_currentMillis - _lastMillis) > ENQ_POKE_INTERVAL_MS) {
      _interface->write(&internals::ProtocolBytes.EOT, 1);
      _lastMillis = _currentMillis;
    }
  }
}

void GWGEngine::_send() {
  _bytesTransferred +=
      _interface->write(&_currentRequest[_bytesTransferred], _currentRequest.length() - _bytesTransferred);
  if (_bytesTransferred == _currentRequest.length()) {
    _bytesTransferred = 0;
    _lastMillis = _currentMillis;
    _sendCompleteMillis = _currentMillis;  // timing instrument (see OnTimingCallback)
    _setState(State::RECEIVE);
  }
}

void GWGEngine::_receive() {
  // Wire-activity deadline (see GWGEngine::RESPONSE_TIMEOUT_MS). This MUST run
  // before the drain below, not after it.
  //
  // The drain sets _lastMillis on every byte it consumes, so a check placed
  // after it can never fire in an iteration that read anything -- and
  // _currentMillis is sampled once per loop(), so bytes that arrived during a
  // long host stall are still "fresh" by the time we look at them. On an ESP32
  // the UART RX FIFO holds the device's idle ENQ across such a stall: drained
  // first, it completes a 1-byte read with 0x05 as the payload, which is
  // exactly the misread this deadline exists to stop, in exactly the long-loop
  // conditions (60 s entity publishes, API state flush) the hardware capture
  // blamed. Checking first means anything still buffered past the deadline is
  // treated as stale, which is the only safe direction: GWG carries no framing
  // and no arrival timestamps, so a buffered 0x05 and a buffered genuine
  // datapoint of value 0x05 are indistinguishable. The cost is a discarded
  // genuine response when the host stalls >250 ms; the alternative is
  // publishing a sync byte as a temperature.
  if (_currentMillis - _lastMillis > RESPONSE_TIMEOUT_MS) {
    // Drop whatever is buffered along with the transaction. Every byte in there
    // predates the deadline, so a stale ENQ left behind would be picked up by
    // _init() as a fresh sync byte and burn the next request on a window that
    // has already closed.
    while (_interface->available())
      _interface->read();
    _bytesTransferred = 0;
    // Out of sync: we no longer know where the device's ENQ cadence is, so the
    // next request waits for a real one rather than riding a stale window.
    _syncValid = false;
    if (_sentInBurst && !_burstDisabled && ++_burstFailures >= BURST_FAILURES_BEFORE_FALLBACK) {
      _burstDisabled = true;
      optolink_log_w("GWG burst disabled: %u consecutive burst-sent requests went unanswered; "
                     "falling back to one telegram per ENQ",
                     static_cast<unsigned>(_burstFailures));
    }
    _setState(State::INIT);
    _tryOnError(OptolinkResult::TIMEOUT);
    return;
  }
  while (_interface->available()) {
    _responseBuffer[_bytesTransferred] = _interface->read();
    ++_bytesTransferred;
    _lastMillis = _currentMillis;
  }
  // Response-completion fix vs. upstream (THIRD_PARTY.md #8): upstream compared
  // the received byte count against the REQUEST frame length (5 for a read,
  // len+5 for a write), so a GWG read of any length != 5 could never complete
  // and timed out. The device actually returns exactly the datapoint's data
  // bytes for a read (vcontrold GWG getaddr: "SEND 01 CB $addr $hexlen 04;
  // RECV $len" -- source-confirmed) and, per the KW-family convention, a
  // single ack byte for a write (model-derived for GWG itself -- vcontrold's
  // GWG setaddr entry is a stub -- but the 1-byte-ack convention is hardware-
  // confirmed on the KW sibling protocol, THIRD_PARTY.md #11; GWG remains
  // unverified on hardware).
  const uint8_t expected = isKnownGwgWriteType(_currentRequest.packetType()) ? 1 : _currentLength;
  if (_bytesTransferred == expected) {
    _bytesTransferred = 0;  // VS1 parity; _init() also resets before SEND
    // A completed transaction is itself proof the device is listening, so it
    // re-opens the burst window -- this is what makes a chain of polls run
    // back-to-back instead of one per ENQ. Mirrors vogod, which re-stamps
    // lastEnq at the end of every completed receive.
    _lastSyncMillis = _currentMillis;
    _syncValid = true;
    if (_sentInBurst)
      _burstFailures = 0;
    _setState(State::INIT);
    // Timing instrument (2026-08-24, see OnTimingCallback): reads only.
    // Writes are out of scope -- GWG write framing remains model-derived
    // (THIRD_PARTY.md #8) and this instrument was motivated by, and only
    // validated against, the read path's ENQ-misread capture.
    if (_onTimingCallback && !isKnownGwgWriteType(_currentRequest.packetType())) {
      _onTimingCallback(_enqAgeMillis, _currentMillis - _sendCompleteMillis, _currentAddress);
    }
    _tryOnResponse(expected);
  }
}

void GWGEngine::_tryOnResponse(uint8_t length) {
  if (_onResponseCallback) {
    _onResponseCallback(_responseBuffer.data(), length, _currentAddress);
  }
  // Bugfix vs. upstream: clear the in-flight state after a successful response,
  // matching VS1/VS2. Without this, GWG refuses every read/write after the
  // first success (one-shot). See THIRD_PARTY.md.
  _busy = false;
}

void GWGEngine::_tryOnError(OptolinkResult result) {
  if (_onErrorCallback) {
    _onErrorCallback(result, _currentAddress);
  }
  _busy = false;
}

}  // namespace esphome::vitohome::optolink
