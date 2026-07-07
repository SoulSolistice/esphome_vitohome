#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "optolink/optolink.h"
#include "response_view.h"

namespace esphome::vitohome {

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

// The single place that knows about the engine's response-callback shape.
// Everything above it -- the hub, the entities and the ESPHome codegen -- sees
// only ResponseView and these uniform methods. All three engines now speak the
// same byte-mover callback (data, length, address), so there is no
// protocol-specific branch here anymore: P300 echoes the address from its
// response frame, KW/GWG echo the request address the engine retained.
class ProtocolAdapter {
 public:
  using ResponseHandler = std::function<void(const ResponseView&, uint16_t request_address)>;
  using ErrorHandler = std::function<void(optolink::OptolinkResult, uint16_t request_address)>;

  template <class Iface>
  explicit ProtocolAdapter(Iface* iface) : engine_(iface) {}

  void on_response(ResponseHandler handler) {
    response_handler_ = std::move(handler);
    engine_.onResponse([this](const uint8_t* data, uint8_t length, uint16_t address) {
      this->established_ = true;
      if (this->response_handler_) {
        // `address` is the one echoed in the device's own response frame on
        // P300 (a real wire-level datum); on KW/GWG the engine echoes the
        // request address it retained. Either way the hub matches it against
        // the address it dispatched. A P300 write ack carries data()==nullptr
        // with a non-zero dataLength(); ResponseView forwards both and the
        // hub's write path does not dereference the payload.
        this->response_handler_(ResponseView{data, length, address}, address);
      }
    });
  }

  void on_error(ErrorHandler handler) {
    error_handler_ = std::move(handler);
    engine_.onError([this](optolink::OptolinkResult error, uint16_t request_address) {
      if (this->error_handler_) {
        this->error_handler_(error, request_address);
      }
    });
  }

  bool begin() { return engine_.begin(); }
  void loop() { engine_.loop(); }
  bool read(const optolink::Datapoint& datapoint) { return engine_.read(datapoint.address(), datapoint.length()); }
  bool write(const optolink::Datapoint& datapoint, const uint8_t* data, uint8_t length) {
    return engine_.write(datapoint.address(), data, length);
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

}  // namespace esphome::vitohome
