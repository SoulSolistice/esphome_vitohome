#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "decode.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome";

// Whether the selected protocol reaches a raw_op layer (queue_raw_op()).
static constexpr bool raw_layer_available(RawLayer layer) {
  return (layer == RawLayer::P300 && IS_P300) || (layer == RawLayer::KW && (IS_KW || IS_P300)) ||
         (layer == RawLayer::GWG && IS_GWG);
}

void VitoHomeComponent::raw_label_(char *out, std::size_t cap, bool is_write, AddressSpace space, RawLayer layer,
                                   uint8_t code) {
  if (layer != RawLayer::NONE) {
    snprintf(out, cap, "%s %02X %s", raw_layer_name(layer), code, is_write ? "write" : "read");
    return;
  }
  const char *kind = space == AddressSpace::RPC   ? "rpc"
                     : space == AddressSpace::GFA ? "gfa read"
                     : is_write                   ? "write"
                                                  : "read";
  snprintf(out, cap, "%s", kind);
}

// ---------------------------------------------------------------------------
// Raw scan console (debug)
// ---------------------------------------------------------------------------

void VitoHomeComponent::queue_raw_read(uint16_t address, uint8_t length, optolink::GWGAccessMode access) {
  if (length < 1 || length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_read: length %u out of range (1..%u)", length, RAW_READ_MAX);
    return;
  }

  this->enqueue_raw_(address, length, false, nullptr, 0, space_from_gwg(access));
}

void VitoHomeComponent::queue_raw_write(uint16_t address, const uint8_t *data, std::size_t len,
                                        optolink::GWGAccessMode access) {
  // The 32-byte cap also keeps packet-length arithmetic safe: the VS2 length
  // byte is 0x05 + len and the VS1 frame length is payload + 4. See the packet
  // implementations before raising this cap.
  if (data == nullptr || len == 0 || len > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "queue_raw_write: %zu bytes out of range (1..%u)", len, RAW_WRITE_MAX);
    return;
  }

  this->enqueue_raw_(address, static_cast<uint8_t>(len), true, data, static_cast<uint8_t>(len), space_from_gwg(access));
}

void VitoHomeComponent::queue_raw_write(uint16_t address, const std::vector<uint8_t> &bytes,
                                        optolink::GWGAccessMode access) {
  // Ergonomic overload for callers that already hold a vector (e.g. a lambda's
  // braced-init-list). Forwards to the pointer/length form so both share one
  // validation-and-enqueue path.
  this->queue_raw_write(address, bytes.data(), bytes.size(), access);
}

void VitoHomeComponent::queue_raw_rpc(uint16_t address, const uint8_t *parameter, std::size_t len) {
  if (!space_available(AddressSpace::RPC)) {
    ESP_LOGW(TAG, "queue_raw_rpc: 0x%04X not sent -- %s has no remote procedure call telegram", address, PROTOCOL_NAME);
    return;
  }
  // Same cap as a raw write: the parameter rides in RawOp::bytes. Zero is
  // allowed -- a procedure without a parameter -- and then runs the procedure.
  if (len > RAW_WRITE_MAX || (len != 0 && parameter == nullptr)) {
    ESP_LOGW(TAG, "queue_raw_rpc: %zu parameter bytes out of range (0..%u)", len, RAW_WRITE_MAX);
    return;
  }
  this->enqueue_raw_(address, static_cast<uint8_t>(len), false, parameter, static_cast<uint8_t>(len),
                     AddressSpace::RPC);
}

void VitoHomeComponent::queue_raw_rpc(uint16_t address, const std::vector<uint8_t> &parameter) {
  this->queue_raw_rpc(address, parameter.data(), parameter.size());
}

void VitoHomeComponent::queue_raw_gfa_read(uint16_t address, uint8_t length) {
  if (!space_available(AddressSpace::GFA)) {
    ESP_LOGW(TAG, "queue_raw_gfa_read: 0x%04X not sent -- %s has no GFA read telegram", address, PROTOCOL_NAME);
    return;
  }
  if (length < 1 || length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_gfa_read: length %u out of range (1..%u)", length, RAW_READ_MAX);
    return;
  }
  this->enqueue_raw_(address, length, false, nullptr, 0, AddressSpace::GFA);
}

bool VitoHomeComponent::queue_raw_op(const char *spec) {
  RawSpec parsed;
  const char *error = nullptr;
  if (!parse_raw_spec(spec, parsed, error, RAW_READ_MAX)) {
    ESP_LOGW(TAG, "queue_raw_op: '%s' -- %s", spec != nullptr ? spec : "", error);
    return false;
  }
  return this->queue_raw_op(parsed.layer, parsed.code, parsed.address, parsed.length,
                            parsed.has_payload ? parsed.payload : nullptr);
}

bool VitoHomeComponent::queue_raw_op(RawLayer layer, uint8_t code, uint16_t address, uint8_t length,
                                     const uint8_t *payload) {
#ifndef VITOHOME_RAW_UNRESTRICTED
  ESP_LOGW(TAG, "queue_raw_op: refused -- set raw_access: unrestricted on the vitohome hub to send any telegram");
  (void) layer;
  (void) code;
  (void) address;
  (void) length;
  (void) payload;
  return false;
#else
  if (!raw_layer_available(layer)) {
    ESP_LOGW(TAG, "queue_raw_op: %s has no %s layer", PROTOCOL_NAME, raw_layer_name(layer));
    return false;
  }
  const RawClass cls = raw_op_class(layer, code, payload != nullptr);
#ifndef VITOHOME_RAW_UNSAFE_WRITES
  if (cls == RawClass::UNSAFE) {
    ESP_LOGW(TAG,
             "queue_raw_op: %s %02X %s refused -- it may change the controller; set unsafe_raw_writes: true to "
             "send it",
             raw_layer_name(layer), code, payload != nullptr ? "with payload" : "without payload");
    return false;
  }
#endif
  if (cls == RawClass::UNSAFE || cls == RawClass::PROBE) {
    ESP_LOGW(TAG, "queue_raw_op: sending %s %s %02X to 0x%04X", raw_class_name(cls), raw_layer_name(layer), code,
             address);
  }
  if (payload != nullptr) {
    if (length == 0 || length > RAW_WRITE_MAX) {
      ESP_LOGW(TAG, "queue_raw_op: %u payload bytes out of range (1..%u)", length, RAW_WRITE_MAX);
      return false;
    }
    return this->enqueue_raw_(address, length, true, payload, length, AddressSpace::OWN, layer, code);
  }
  const bool call = layer == RawLayer::P300 && code == static_cast<uint8_t>(optolink::FunctionCode::RPC);
  if ((length == 0 && !call) || length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_op: length %u out of range (1..%u)", length, RAW_READ_MAX);
    return false;
  }
  return this->enqueue_raw_(address, length, false, nullptr, 0, AddressSpace::OWN, layer, code);
#endif
}

bool VitoHomeComponent::enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t *bytes,
                                     uint8_t bytes_len, AddressSpace space, RawLayer layer, uint8_t code) {
  const bool is_rpc = space == AddressSpace::RPC ||
                      (layer == RawLayer::P300 && code == static_cast<uint8_t>(optolink::FunctionCode::RPC));
  char label[24];
  raw_label_(label, sizeof(label), is_write, space, layer, code);
  // An RPC's length is its parameter's, which may be empty; see queue_raw_rpc().
  if (length == 0 && !is_rpc) {
    ESP_LOGW(TAG, "enqueue_raw_: zero-length operation rejected");
    return false;
  }

  if (bytes_len > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "enqueue_raw_: %u bytes exceeds %u; dropping", bytes_len, RAW_WRITE_MAX);
    return false;
  }

  if (layer == RawLayer::NONE && (!space_available(space) || (is_write && !space_writable(space)))) {
    ESP_LOGW(TAG, "enqueue_raw_: %s has no %s telegram in access space '%s'", PROTOCOL_NAME,
             is_write ? "write" : "read", space_name(space));
    return false;
  }
  if (is_rpc && !is_write) {
    if (bytes_len != length || (bytes_len != 0 && bytes == nullptr)) {
      ESP_LOGW(TAG, "enqueue_raw_: invalid rpc parameter (length=%u, bytes_len=%u)", length, bytes_len);
      return false;
    }
  } else if (is_write) {
    if (bytes == nullptr || bytes_len == 0 || bytes_len != length) {
      ESP_LOGW(TAG, "enqueue_raw_: invalid write payload (length=%u, bytes_len=%u, data=%s)", length, bytes_len,
               bytes == nullptr ? "null" : "set");
      return false;
    }
  } else if (bytes != nullptr || bytes_len != 0) {
    ESP_LOGW(TAG, "enqueue_raw_: read operation carried a payload");
    return false;
  }

  RawOp operation{};
  operation.address = address;
  operation.length = length;
  operation.is_write = is_write;
  operation.bytes_len = bytes_len;
  operation.space = space;
  operation.layer = layer;
  operation.code = code;

  if ((is_write || is_rpc) && bytes_len != 0)
    std::memcpy(operation.bytes, bytes, bytes_len);

  // push_back() is authoritative. A preceding full() check would be a
  // check/use race when multiple producers are possible.
  // capacity 0 is the default: the scan console is opt-in, so an unallocated
  // lane is the normal state, not an error. Name the option in the message --
  // this is the only feedback a user gets for "my scan button does nothing".
  if (!this->raw_queue_.push_back(operation)) {
    if (this->raw_queue_.capacity() == 0) {
      ESP_LOGW(TAG,
               "raw lane not allocated; dropping %s 0x%04X. Set raw_queue_size on the vitohome hub to use the "
               "scan console.",
               label, address);
    } else {
      ESP_LOGW(TAG, "raw queue full (%zu/%zu); dropping %s 0x%04X. Raise raw_queue_size for larger sweeps.",
               this->raw_queue_.size(), this->raw_queue_.capacity(), label, address);
    }

    return false;
  }

  ESP_LOGD(TAG, "Queued raw %s 0x%04X len %u", label, address, length);
  return true;
}

void VitoHomeComponent::raw_handle_response_(const ResponseView &response) {
  // "0xXXXX:" (7) + RAW_READ_MAX * 3 hex chars + the integer views for widths
  // 1..8 + NUL. 208 covers a 48-byte dump with room to spare; the label and
  // the time ride in front.
  char buffer[240];
  int offset = 0;
  const uint32_t elapsed = millis() - this->in_flight_started_ms_;

  if (this->raw_layer_ != RawLayer::NONE) {
    char label[24];
    raw_label_(label, sizeof(label), this->raw_is_write_, this->raw_space_, this->raw_layer_, this->raw_code_);
    offset = snprintf(buffer, sizeof(buffer), "%s, %" PRIu32 " ms: ", label, elapsed);
    if (offset < 0 || offset >= static_cast<int>(sizeof(buffer)))
      offset = 0;
  }
  if (this->raw_is_write_ && this->raw_layer_ == RawLayer::NONE) {
    const int written = snprintf(buffer, sizeof(buffer), "0x%04X: write ACK (%u byte%s)", this->raw_dp_.address(),
                                 this->raw_write_len_, this->raw_write_len_ == 1 ? "" : "s");
    // KW and GWG acknowledge a write with one byte. KW's is 0x00, checked by
    // the engine. GWG's value is not known -- no GWG write has been confirmed
    // -- and its engine completes on any byte, so the byte is shown: it is
    // what a first write test has to report. A P300 acknowledgement frame
    // carries no data.
    if (response.data != nullptr && response.data_length >= 1 && written > 0 &&
        written < static_cast<int>(sizeof(buffer)))
      snprintf(buffer + written, sizeof(buffer) - written, ", answered %02X", response.data[0]);
  } else if (response.data == nullptr || response.data_length == 0) {
    // A P300 write acknowledgement carries no data.
    snprintf(buffer + offset, sizeof(buffer) - offset, "0x%04X: ACK, no data", response.address);
  } else {
    format_raw_dump(response.address, response.data, response.data_length, buffer + offset, sizeof(buffer) - offset);
  }

  ESP_LOGI(TAG, "Raw: %s", buffer);
  this->raw_publish_(buffer);
}

void VitoHomeComponent::raw_handle_error_(optolink::OptolinkResult error) {
  char buffer[112];
  char label[24];
  raw_label_(label, sizeof(label), this->raw_is_write_, this->raw_space_, this->raw_layer_, this->raw_code_);

  int offset = snprintf(buffer, sizeof(buffer), "0x%04X: %s FAILED (%s", this->raw_dp_.address(), label,
                        optolink::errorToString(error));
#ifndef VITOHOME_PROTOCOL_KW
#ifndef VITOHOME_PROTOCOL_GWG
  // The controller's error code from the P300 error frame: 01 an address it
  // does not serve, 05 a function code it lacks (VS2Engine::errorPayload()).
  if (error == optolink::OptolinkResult::DEVICE_ERROR) {
    uint8_t length = 0;
    const uint8_t *code = this->vito_->errorPayload(length);
    for (uint8_t i = 0; i < length && offset > 0 && offset < static_cast<int>(sizeof(buffer)); ++i)
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, i == 0 ? ", code %02X" : " %02X", code[i]);
  }
#endif
#endif
  if (offset > 0 && offset < static_cast<int>(sizeof(buffer)))
    snprintf(buffer + offset, sizeof(buffer) - offset, ")");

  ESP_LOGW(TAG, "Raw: %s", buffer);
  this->raw_publish_(buffer);
}

void VitoHomeComponent::raw_publish_(const char *line) {
#ifdef USE_TEXT_SENSOR
  // The callers all pass a fixed char buffer; publish_state(const char *)
  // assigns into each sensor's reused state string, whereas a const std::string
  // & parameter would construct (and heap-allocate) a std::string per call.
  for (uint16_t i = 0; i < this->raw_result_sensor_count_; i++) {
    if (this->raw_result_sensors_[i] != nullptr)
      this->raw_result_sensors_[i]->publish_state(line);
  }
#else
  (void) line;
#endif
}

void VitoHomeComponent::dispatch_raw_front_() {
  // Peek, hand over to the engine, then remove: the operation stays at the head
  // of the lane until read()/write() has accepted it. The engine copies a write
  // payload into its own fixed packet synchronously inside write().
  //
  // No lock is held across the hand-off -- each RingBuffer call locks only for
  // its own duration -- so a refusal is reported straight from here, although
  // publishing a result runs the scan_result sensors' automations, which may
  // queue the next raw operation. (The lane's old consume_front_if() ran this
  // body under the queue's non-recursive mutex, where that was a deadlock;
  // external review, 2026-10-28.) The head cannot change in between: only this
  // loop removes from the lane, and a producer on another task appends.
  RawOp operation{};
  if (!this->raw_queue_.try_front(operation))
    return;
  {
    this->raw_dp_ = optolink::Datapoint("scan", operation.address, operation.length, optolink::noconv);

    this->raw_is_write_ = operation.is_write;
    this->raw_space_ = operation.space;
    this->raw_layer_ = operation.layer;
    this->raw_code_ = operation.code;

    // The space picks the telegram (send_read_ / send_write_): a GWG access
    // mode on GWG -- the scan console can pick EEPROM (0xAD) / BE (0x9D), the
    // modes Vitosoft marks writable --, a remote procedure call with its
    // parameter, a GFA read. OWN, the protocol's own read or write, unless
    // the caller asked for another.
    // A queue_raw_op() operation names its layer and code itself.
    const bool dispatched =
        operation.layer != RawLayer::NONE
            ? this->send_raw_op_(operation.address, operation.length, operation.layer, operation.code,
                                 operation.is_write ? operation.bytes : nullptr, operation.bytes_len)
        : operation.is_write
            ? this->send_write_(operation.address, operation.bytes, operation.bytes_len, operation.space)
            : this->send_read_(operation.address, operation.length, operation.space, operation.bytes,
                               operation.bytes_len);
    char label[24];
    raw_label_(label, sizeof(label), operation.is_write, operation.space, operation.layer, operation.code);

    if (!dispatched) {
      // read()/write() returning false is overloaded: transient backpressure
      // (the engine is _busy with an in-flight transaction -- retry next loop)
      // versus a permanent createPacket() rejection for this (address, length,
      // type). The live permanent case is a raw op with a 16-bit address on a
      // GWG device (PacketGWG rejects addr > 0xFF); every other createPacket
      // failure mode is blocked by the schema and the RAW_*_MAX caps. Retaining
      // the head on a permanent refusal stalls this lane forever, and because
      // dispatch_next_() services the raw queue before the read/write lanes and
      // returns, it silently freezes ALL bus traffic until reboot -- with
      // nothing in flight for IN_FLIGHT_WATCHDOG_MS to catch. Distinguish the
      // two with isBusy() and drop a permanently-refused op so the lane moves.
      if (this->vito_->isBusy())
        return;  // transient: keep the item at the head and retry
      ESP_LOGE(TAG, "Engine refused raw %s 0x%04X len %u (permanent); dropping", label, operation.address,
               operation.length);
      this->raw_queue_.drop_front();  // permanent: drop so the read/write lanes are reached
      this->raw_handle_error_(optolink::OptolinkResult::ERROR);
      return;
    }

    this->raw_queue_.drop_front();
    this->raw_write_len_ = operation.is_write ? operation.bytes_len : 0;
    // A KW telegram over P300 rides the same detour as a GFA read, so it
    // counts as one for the address-space affinity.
    this->last_space_ = operation.layer == RawLayer::KW && space_has_switch_cost(AddressSpace::GFA) ? AddressSpace::GFA
                                                                                                    : operation.space;
    // A GWG EEPROM read, through its access mode or the generic route's 0xAE,
    // takes one telegram per address.
    const bool gwg_eeprom_type = operation.layer == RawLayer::GWG &&
                                 operation.code == optolink::gwgReadTypeByte(optolink::GWGAccessMode::EEPROM);
    this->set_in_flight_(
        OpType::RAW, nullptr,
        operation.is_write
            ? 1
            : read_telegram_count(gwg_eeprom_type ? AddressSpace::GWG_EEPROM : operation.space, operation.length));

    ESP_LOGV(TAG, "Dispatched raw %s 0x%04X len %u", label, operation.address, operation.length);
  }
}

}  // namespace esphome::vitohome
