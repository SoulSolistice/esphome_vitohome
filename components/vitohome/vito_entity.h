#pragma once
#include <cstring>

#include "optolink/optolink.h"

namespace esphome {
namespace vitohome {

class VitoHomeComponent;

// Common base for any entity that owns an optolink datapoint. The component
// holds a vector<VitoEntityBase*> and dispatches read/write responses and
// errors back to the originating entity via its in-flight pointer. Concrete
// subclasses translate raw Optolink payloads into ESPHome state publishes.
//
// Stage 2 additions:
//  * per-entity poll interval (0 = poll on every hub cycle), scheduled by
//    the hub at hub-tick granularity;
//  * a small write buffer + handle_write_response() hook for the encode
//    path (number/select). Entities fill write_buf_/write_len_ and call
//    VitoHomeComponent::request_write(this); the hub owns bus arbitration.
class VitoEntityBase {
 public:
  virtual ~VitoEntityBase() = default;

  void set_datapoint(const optolink::Datapoint &dp) { this->datapoint_ = dp; }
  const optolink::Datapoint &get_datapoint() const { return this->datapoint_; }

  // Optional distinct write target. When set, polling, read-back and read
  // response-matching still use datapoint_ (the state / read address) but
  // writes go here (the command address) -- for mode controls whose live state
  // is read at a different address than the command register (see the
  // read/write-split analysis). Unset: writes use datapoint_, i.e. the
  // original single-address behaviour, so existing entities are unaffected.
  void set_write_datapoint(const optolink::Datapoint &dp) {
    this->write_datapoint_ = dp;
    this->has_write_dp_ = true;
  }
  const optolink::Datapoint &get_write_datapoint() const {
    return this->has_write_dp_ ? this->write_datapoint_ : this->datapoint_;
  }

  void set_vitohome_parent(VitoHomeComponent *parent) { this->vh_parent_ = parent; }

  // --- scheduling -----------------------------------------------------------
  // 0 (default) = poll on every hub update cycle. Anything else is a minimum
  // period; effective granularity is the hub's own update_interval (the hub
  // warns at setup if an entity interval is shorter than the hub's).
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  uint32_t poll_interval() const { return this->poll_interval_ms_; }

  // Hub-side bookkeeping (only the hub touches these).
  uint32_t next_due_ms_{0};
  bool read_queued_{false};
  bool write_queued_{false};

  // --- read path ------------------------------------------------------------
  // Called by the component on a successful read response. Packet length and
  // checksum have already been verified by the optolink engine.
  virtual void handle_response(const optolink::PacketVS2 &response) = 0;

  // Called by the component on a protocol-level error (read or write).
  virtual void handle_error(optolink::OptolinkResult error) = 0;

  // --- write path -----------------------------------------------------------
  const uint8_t *write_data() const { return this->write_buf_; }
  uint8_t write_length() const { return this->write_len_; }

  // Whether a confirmed write should be followed by an immediate read of the
  // same address (publish the device's view, not our optimistic one).
  bool wants_read_back() const { return this->read_back_; }

  // Called by the component when the device ACKed a write. Default: no-op
  // (the hub enqueues the read-back when wants_read_back()).
  virtual void handle_write_response(const optolink::PacketVS2 & /*response*/) {}

  // --- logging / dump_config --------------------------------------------------
  virtual const char *entity_kind() const = 0;

  // Each concrete entity logs its own config; the component fans out to
  // these from its own dump_config(). Concrete subclasses also inherit a
  // dump_config() from ESPHome's Component, so a single `override` in each
  // satisfies both declarations.
  virtual void dump_config() = 0;

 protected:
  bool set_write_payload_(const uint8_t *data, uint8_t len) {
    if (data == nullptr || len == 0 || len > sizeof(this->write_buf_)) return false;
    std::memcpy(this->write_buf_, data, len);
    this->write_len_ = len;
    return true;
  }

  // Default-constructed Datapoint until set_datapoint runs from codegen.
  // The optolink converter slot is always noconv: vitohome decodes and
  // encodes the raw payload itself (decode.h) and uses the raw-bytes write
  // overload, so the library converter is never exercised.
  optolink::Datapoint datapoint_{"uninitialized", 0, 1, optolink::noconv};
  // Distinct write target (command address); used only when has_write_dp_.
  optolink::Datapoint write_datapoint_{"uninitialized", 0, 1, optolink::noconv};
  bool has_write_dp_{false};

  VitoHomeComponent *vh_parent_{nullptr};
  uint8_t write_buf_[4]{0, 0, 0, 0};
  uint8_t write_len_{0};
  bool read_back_{true};
  uint32_t poll_interval_ms_{0};
};

}  // namespace vitohome
}  // namespace esphome
