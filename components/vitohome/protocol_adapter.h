#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "optolink/optolink.h"
#include "response_view.h"

namespace esphome {
namespace vitohome {

// Compile-time protocol selection. The ESPHome codegen (__init__.py) emits
// exactly one VITOHOME_PROTOCOL_* build flag from the `protocol:` option; the
// default (no flag) is P300, the only protocol exercised on hardware.
#if defined(VITOHOME_PROTOCOL_KW)
using SelectedProtocol = optolink::KW;
inline constexpr const char* PROTOCOL_NAME = "KW (VS1)";
#elif defined(VITOHOME_PROTOCOL_GWG)
using SelectedProtocol = optolink::GWG;
inline constexpr const char* PROTOCOL_NAME = "GWG";
#else
using SelectedProtocol = optolink::P300;
inline constexpr const char* PROTOCOL_NAME = "P300 (VS2)";
#endif

// The single place that knows about engine packet types and the per-protocol
// response-callback shape. Everything above it -- the hub, the entities and the
// ESPHome codegen -- sees only ResponseView and these uniform methods. P300
// goes through this same path as KW and GWG, so there is no protocol-specific
// branch above the adapter.
class ProtocolAdapter {
 public:
  using ResponseHandler = std::function<void(const ResponseView&, const optolink::Datapoint&)>;
  using ErrorHandler = std::function<void(optolink::OptolinkResult, const optolink::Datapoint&)>;

  template <class Iface>
  explicit ProtocolAdapter(Iface* iface) : engine_(iface) {}

  void on_response(ResponseHandler handler) {
    response_handler_ = std::move(handler);
#if defined(VITOHOME_PROTOCOL_KW) || defined(VITOHOME_PROTOCOL_GWG)
    // KW / GWG deliver raw bytes; the request datapoint carries the address.
    engine_.onResponse([this](const uint8_t* data, uint8_t length, const optolink::Datapoint& request) {
      this->established_ = true;
      if (this->response_handler_) {
        this->response_handler_(ResponseView{data, length, request.address()}, request);
      }
    });
#else
    // P300 delivers a PacketVS2; pull the payload out of it. The address is
    // the one ECHOED IN THE RESPONSE FRAME (bytes 3..4), not the request's --
    // the device echoes the address on both read responses and write acks
    // (hardware-confirmed by the transaction-harness fixtures), so the hub's
    // response-address match is a real wire-level check on P300. Previously
    // request.address() was passed here, which made that check compare the
    // request against itself and never fire.
    engine_.onResponse([this](const optolink::PacketVS2& packet, const optolink::Datapoint& request) {
      this->established_ = true;
      if (this->response_handler_) {
        this->response_handler_(ResponseView{packet.data(), packet.dataLength(), packet.address()}, request);
      }
    });
#endif
  }

  void on_error(ErrorHandler handler) {
    error_handler_ = std::move(handler);
    engine_.onError([this](optolink::OptolinkResult error, const optolink::Datapoint& request) {
      if (this->error_handler_) {
        this->error_handler_(error, request);
      }
    });
  }

  bool begin() { return engine_.begin(); }
  void loop() { engine_.loop(); }
  bool read(const optolink::Datapoint& datapoint) { return engine_.read(datapoint); }
  bool write(const optolink::Datapoint& datapoint, const uint8_t* data, uint8_t length) {
    return engine_.write(datapoint, data, length);
  }
  bool is_busy() const { return engine_.isBusy(); }

  // True once the engine has produced at least one valid response since begin().
  // A valid response means the device speaks the configured protocol, so the
  // hub uses this as the start-up verification signal.
  bool established() const { return established_; }

  static const char* protocol_name() { return PROTOCOL_NAME; }

 private:
  optolink::OptolinkEngine<SelectedProtocol> engine_;
  ResponseHandler response_handler_{};
  ErrorHandler error_handler_{};
  bool established_{false};
};

}  // namespace vitohome
}  // namespace esphome
