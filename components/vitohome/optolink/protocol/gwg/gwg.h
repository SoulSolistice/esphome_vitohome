/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Cleanups vs. upstream: platform serial adapters and platform-gated
constructors removed (template<class C> ctor only); engine-level
_responseBuffer malloc + _expandResponseBuffer + _allocatedLength replaced
by a fixed std::array; request timeout lifted to a named constexpr member.
Bugfix: _tryOnResponse() clears the in-flight (busy) state after the
callback (see THIRD_PARTY.md) so GWG is no longer one-shot.
Response deadline: RECEIVE now enforces RESPONSE_TIMEOUT_MS of wire activity
instead of relying solely on the 3000ms request watchdog, so an unanswered
request fails as TIMEOUT rather than being completed by the device's next idle
ENQ (0x05) - hardware-confirmed misread, see that constant.
Send latency: _init() falls through into _send() in the same loop() iteration
that observed the ENQ, instead of deferring the write by one iteration.
Access modes: read() takes an optional GWGAccessMode (default PHYSICAL,
byte-identical to pre-existing behaviour); see constants.h for the mode table
and its evidence.
Timing instrument: an optional onTiming() callback reports per-read
ENQ-to-send and send-to-response deltas in milliseconds, motivated by the same
hardware capture as the response deadline; see OnTimingCallback.
Sync poke: optional EOT (0x04) nudge while waiting for the device ENQ, gated by
GWGEngine::SEND_ENQ_POKE (default off, so the default build is unchanged); see
that flag for the vcontrold/VS1 rationale.
Fail-soft: the constructor no longer abort()s on serial-interface allocation
failure; it leaves the engine in a failed state and begin() returns false (see
THIRD_PARTY.md), so the wrapper mark_failed()s instead of the firmware aborting.
*/

#pragma once

#include <array>
#include <cassert>
#include <functional>

#include "../../constants.h"
#include "../../helpers.h"
#include "../../interface/generic_interface.h"
#include "../../logging.h"
#include "packet_gwg.h"

namespace esphome::vitohome::optolink {

class GWGEngine {
 public:
  // Byte-mover API (see vs2.h). GWG carries no address in the response, so the
  // engine echoes the request address back to the caller unchanged.
  typedef std::function<void(const uint8_t *data, uint8_t length, uint16_t address)> OnResponseCallback;
  typedef std::function<void(OptolinkResult error, uint16_t address)> OnErrorCallback;
  // Diagnostic instrument, GWG-only (2026-08-24, motivated by the same
  // hardware capture as RESPONSE_TIMEOUT_MS). Fires once per successfully
  // completed READ, never for writes or failures. enq_to_send_ms measures
  // _init()'s ENQ-observed moment to _send()'s frame-fully-written moment --
  // whether the same-loop fall-through (see gwg.cpp) is actually keeping the
  // KW-family sync window, which host-batched API log timestamps could not
  // resolve (see gwg.cpp _init(), "Send latency"). send_to_response_ms
  // measures frame-fully-written to response-fully-received -- the real
  // distribution RESPONSE_TIMEOUT_MS=250 was picked against unreliable
  // timestamps for; this gives the true number on real hardware. No callback
  // registered (the default) costs nothing beyond the two millis() snapshots
  // already needed internally.
  typedef std::function<void(uint32_t enq_to_send_ms, uint32_t send_to_response_ms, uint16_t address)> OnTimingCallback;

  // Named timeout (ms). GWG deliberately uses a 3000ms request watchdog,
  // distinct from VS2/VS1's 4000ms - value byte-identical to upstream. It is
  // measured from read()/write() (i.e. it also covers the ENQ wait in INIT),
  // which is what makes RESPONSE_TIMEOUT_MS below necessary as well.
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;

  // Wire-activity deadline inside RECEIVE (ms), measured from the end of the
  // request write and re-armed on every received byte.
  //
  // Divergence from upstream, hardware-motivated. GWG has no framing: a read
  // response is exactly _currentLength raw data bytes with no start byte, type
  // byte or checksum, so the engine cannot tell payload from any other byte on
  // the wire. A KW-family device also emits an unsolicited ENQ (0x05) roughly
  // once a second while idle. With only the 3000ms REQUEST_TIMEOUT_MS guarding
  // RECEIVE, a request the device ignored was silently completed by the NEXT
  // idle ENQ, and 0x05 was handed to the caller as data - i.e. 2.5 degC on any
  // 0.5-scaled temperature datapoint, reported as success, with no warning.
  // Hardware-confirmed on a GWG unit (2026-08-23 capture): 9 of 92 reads
  // returned exactly 0x05 after 903-1476 ms, against 0-90 ms for the 79 reads
  // that the device actually answered.
  //
  // Dropping a leading 0x05 is NOT a valid fix: 0x05 is a legal datapoint
  // value. Timing is the only discriminator available, and it separates the two
  // populations cleanly. 250 ms sits ~2.7x above the slowest observed genuine
  // response and ~3x below the shortest observed idle-ENQ gap (749 ms), and is
  // ~22x the 11.5 ms it takes to shift a 5-byte request out at 4800 8E2.
  // Expiry raises OptolinkResult::TIMEOUT, which the ESPHome hub already treats
  // as a link-health event and which suppresses the bogus publish.
  //
  // Cross-validated 2026-08-23 against dannerph/esphome_vitoconnect (GWG_RX_
  // TOTAL_TIMEOUT_MS=800, GWG_RX_INTERBYTE_TIMEOUT_MS=80, both hardware-tested)
  // and the openv wiki (Protokoll-GWG: device sends 0x05 periodically, and a
  // telegram sent immediately after it is answered immediately -- the timing
  // assumption this constant encodes is spec, not inference).
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 250;

  // Fixed response buffer: bounds the largest GWG datapoint payload. 256 is a
  // safe upper bound (datapoint length is a uint8_t).
  static constexpr std::size_t kResponseBufferSize = 256;

  // Active sync poke. OFF by default: INIT waits passively for the device's ENQ
  // (0x05), byte-identical to the original behaviour. Set to true only if a GWG
  // device never establishes -- then INIT also sends an EOT (0x04) every
  // ENQ_POKE_INTERVAL_MS while waiting, mirroring vcontrold's GWG sync
  // (SEND 04; WAIT 05) and the VS1 engine's EOT fallback. Needs a real GWG unit
  // to validate; GWG is an untested protocol.
  static constexpr bool SEND_ENQ_POKE = false;
  static constexpr uint32_t ENQ_POKE_INTERVAL_MS = 3000;

  template<class C>
  explicit GWGEngine(C *interface)
      : _state(State::UNDEFINED),
        _currentMillis(optolink_millis()),
        _lastMillis(_currentMillis),
        _requestTime(0),
        _enqMillis(0),
        _sendCompleteMillis(0),
        _bytesTransferred(0),
        _interface(nullptr),
        _currentAddress(0),
        _currentLength(0),
        _busy(false),
        _currentRequest(),
        _responseBuffer{},
        _onResponseCallback(nullptr),
        _onErrorCallback(nullptr),
        _onTimingCallback(nullptr) {
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
  ~GWGEngine();
  GWGEngine(const GWGEngine &) = delete;
  GWGEngine &operator=(const GWGEngine &) = delete;

  void onResponse(OnResponseCallback callback);
  void onError(OnErrorCallback callback);
  void onTiming(OnTimingCallback callback);

  // access is additive (default GWGAccessMode::PHYSICAL): every existing
  // 2-argument call site is source- and behaviour-compatible, and produces
  // the exact same TYPE byte (0xCB) as before this parameter existed. See
  // GWGAccessMode in constants.h for the full mode table and its evidence.
  bool read(uint16_t address, uint8_t length, GWGAccessMode access = GWGAccessMode::PHYSICAL);
  bool write(uint16_t address, const uint8_t *data, uint8_t length);

  bool begin();
  void loop();
  void end();

  bool isBusy() const;

 private:
  enum class State { INIT, SEND, RECEIVE, UNDEFINED } _state;
  uint32_t _currentMillis;
  uint32_t _lastMillis;
  uint32_t _requestTime;
  // Timing instrument snapshots (see OnTimingCallback above). _enqMillis is
  // stamped in _init() when the device's ENQ is observed; _sendCompleteMillis
  // is stamped in _send() when the full request frame has been written. Both
  // are scratch state read only inside the completion branch of _receive()
  // and are safe to leave stale between transactions -- they are never read
  // except immediately after being freshly set for the in-flight request.
  uint32_t _enqMillis;
  uint32_t _sendCompleteMillis;
  uint8_t _bytesTransferred;
  internals::SerialInterface *_interface;
  uint16_t _currentAddress;
  uint8_t _currentLength;
  bool _busy;
  PacketGWG _currentRequest;
  std::array<uint8_t, kResponseBufferSize> _responseBuffer;
  OnResponseCallback _onResponseCallback;
  OnErrorCallback _onErrorCallback;
  OnTimingCallback _onTimingCallback;

  inline void _setState(State state);

  void _init();
  void _send();
  void _receive();

  void _tryOnResponse(uint8_t length);
  void _tryOnError(OptolinkResult result);
};

}  // namespace esphome::vitohome::optolink
