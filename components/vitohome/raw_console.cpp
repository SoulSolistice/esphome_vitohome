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

// The layer the link speaks, and so the only one an open frame can take.
static constexpr RawLayer link_layer() { return IS_GWG ? RawLayer::GWG : IS_KW ? RawLayer::KW : RawLayer::P300; }

void VitoHomeComponent::raw_label_(char *out, std::size_t cap, bool is_write, AddressSpace space, RawLayer layer,
                                   uint8_t code, bool frame) {
  if (frame) {
    snprintf(out, cap, "%s frame", raw_layer_name(layer));
    return;
  }
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
  // The cap also keeps the packets' length arithmetic in range (VS2 length byte
  // 0x05 + len, VS1 frame payload + 4); check the packets before raising it.
  if (data == nullptr || len == 0 || len > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "queue_raw_write: %zu bytes out of range (1..%u)", len, RAW_WRITE_MAX);
    return;
  }

  this->enqueue_raw_(address, static_cast<uint8_t>(len), true, data, static_cast<uint8_t>(len), space_from_gwg(access));
}

void VitoHomeComponent::queue_raw_write(uint16_t address, const std::vector<uint8_t> &bytes,
                                        optolink::GWGAccessMode access) {
  this->queue_raw_write(address, bytes.data(), bytes.size(), access);
}

void VitoHomeComponent::queue_raw_rpc(uint16_t address, const uint8_t *parameter, std::size_t len) {
  if (!space_available(AddressSpace::RPC)) {
    ESP_LOGW(TAG, "queue_raw_rpc: 0x%04X not sent -- %s has no remote procedure call telegram", address, PROTOCOL_NAME);
    return;
  }
  // The parameter is kept in RawOp::bytes, so a write's cap. Zero bytes is a
  // procedure without a parameter.
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
  if (parsed.frame) {
    if (parsed.layer != link_layer()) {
      ESP_LOGW(TAG, "queue_raw_op: '%s' -- a %s frame on a %s link; a frame takes the link's protocol", spec,
               raw_layer_name(parsed.layer), PROTOCOL_NAME);
      return false;
    }
    if (raw_frame_timed(parsed.waits, parsed.now, parsed.answer_p300))
      return this->queue_raw_script_(parsed);
    return this->queue_raw_frame(parsed.payload, parsed.frame_len, parsed.length);
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
  const RawClass cls = raw_op_class(layer, code, payload != nullptr ? length : 0);
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

bool VitoHomeComponent::queue_raw_frame(const uint8_t *bytes, std::size_t len, uint8_t answer_length) {
#if !defined(VITOHOME_RAW_UNRESTRICTED) || !defined(VITOHOME_RAW_UNSAFE_WRITES)
  ESP_LOGW(TAG, "queue_raw_frame: refused -- an open frame needs raw_access: unrestricted and unsafe_raw_writes: true "
                "on the vitohome hub");
  (void) bytes;
  (void) len;
  (void) answer_length;
  return false;
#else
  if (bytes == nullptr || len == 0 || len > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "queue_raw_frame: %zu bytes out of range (1..%u)", len, RAW_WRITE_MAX);
    return false;
  }
  if (answer_length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_frame: answer length %u out of range (0..%u)", answer_length, RAW_READ_MAX);
    return false;
  }
  char label[24];
  raw_label_(label, sizeof(label), false, AddressSpace::OWN, link_layer(), 0, true);
  ESP_LOGW(TAG, "queue_raw_frame: sending unsafe %s of %zu bytes", label, len);
  RawOp operation{};
  operation.length = answer_length;
  operation.bytes_len = static_cast<uint8_t>(len);
  std::memcpy(operation.bytes, bytes, len);
  operation.layer = link_layer();
  operation.frame = true;
  return this->push_raw_(operation, label);
#endif
}

bool VitoHomeComponent::queue_raw_script_(const RawSpec &spec) {
#ifndef VITOHOME_RAW_SCRIPT
  ESP_LOGW(TAG, "queue_raw_op: a timed frame needs raw_access: unrestricted and unsafe_raw_writes: true on the "
                "vitohome hub");
  (void) spec;
  return false;
#else
  if (spec.frame_len > RAW_WRITE_MAX || spec.length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_op: timed frame of %u bytes, answer %u: out of range", spec.frame_len, spec.length);
    return false;
  }
  char label[24];
  raw_label_(label, sizeof(label), false, AddressSpace::OWN, spec.layer, 0, true);
  ESP_LOGW(TAG, "queue_raw_op: sending unsafe timed %s of %u bytes; the engine pauses for it", label, spec.frame_len);
  RawOp operation{};
  operation.length = spec.length;
  operation.bytes_len = spec.frame_len;
  std::memcpy(operation.bytes, spec.payload, spec.frame_len);
  operation.layer = spec.layer;
  operation.frame = true;
  operation.waits = spec.waits;
  std::memcpy(operation.wait_at, spec.wait_at, sizeof(operation.wait_at));
  std::memcpy(operation.wait_ms, spec.wait_ms, sizeof(operation.wait_ms));
  operation.now = spec.now;
  operation.answer_p300 = spec.answer_p300;
  return this->push_raw_(operation, label);
#endif
}

RawSpec VitoHomeComponent::raw_frame_spec_(const RawOp &operation) {
  RawSpec spec{};
  spec.layer = operation.layer;
  spec.frame = true;
  spec.frame_len = operation.bytes_len;
  std::memcpy(spec.payload, operation.bytes, operation.bytes_len);
  spec.length = operation.length;
  spec.waits = operation.waits;
  std::memcpy(spec.wait_at, operation.wait_at, sizeof(spec.wait_at));
  std::memcpy(spec.wait_ms, operation.wait_ms, sizeof(spec.wait_ms));
  spec.now = operation.now;
  spec.answer_p300 = operation.answer_p300;
  return spec;
}

void VitoHomeComponent::raw_describe_(const RawOp &operation, const char *label, char *out, std::size_t cap) {
  if (operation.frame) {
    std::size_t n = 0;
    out[0] = '\0';
    format_raw_frame_spec(raw_frame_spec_(operation), out, cap, n);
  } else {
    snprintf(out, cap, "%s 0x%04X len %u", label, operation.address, operation.length);
  }
}

void VitoHomeComponent::raw_script_start_(const RawOp &operation) {
#ifdef VITOHOME_RAW_SCRIPT
  this->raw_script_.start(raw_frame_spec_(operation), this->raw_script_timing_());
  this->raw_script_hold_.start();
#else
  (void) operation;
#endif
}

RawScriptTiming VitoHomeComponent::raw_script_timing_() const {
  // The engines' own answer deadlines; the ENQ wait as long as KW's after an
  // EOT, the whole script well inside the hub's watchdog.
#if defined(VITOHOME_PROTOCOL_GWG)
  const uint32_t quiet = this->gwg_response_timeout_ms_;
#elif defined(VITOHOME_PROTOCOL_KW)
  const uint32_t quiet = optolink::VS1Engine::ANSWER_TIMEOUT_MS;
#else
  const uint32_t quiet = optolink::VS2Engine::FRAME_QUIET_MS;
#endif
  static_assert(RAW_SCRIPT_DEADLINE_MS < IN_FLIGHT_WATCHDOG_MS, "a script must end before the hub's watchdog");
  return RawScriptTiming{RAW_SCRIPT_ENQ_TIMEOUT_MS, quiet, RAW_SCRIPT_DEADLINE_MS};
}

void VitoHomeComponent::raw_script_loop_() {
#ifdef VITOHOME_RAW_SCRIPT
  if (!this->raw_script_.loop())
    return;
  this->raw_script_hold_.stop();
  char line[256];
  format_raw_script(this->raw_script_.spec(), this->raw_script_.result(), this->raw_script_timing_(), line,
                    sizeof(line));
  if (this->raw_script_.result().status == RawScriptStatus::OK) {
    ESP_LOGI(TAG, "Raw: %s", line);
  } else {
    ESP_LOGW(TAG, "Raw: %s", line);
  }
  this->raw_publish_(line);
  this->raw_script_.clear();
  // A fresh start resyncs the link: P300 from its reset, KW and GWG on the
  // device's next ENQ.
  this->vito_->begin();
  VitoEntityBase *entity = nullptr;
  (void) this->take_in_flight_(entity);
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

  return this->push_raw_(operation, label);
}

bool VitoHomeComponent::push_raw_(const RawOp &operation, const char *label) {
  // push_back() decides; a full() check first would race another producer. An
  // unallocated lane is the default, and the warning is the only sign of it,
  // so it names the option.
  char described[160];
  raw_describe_(operation, label, described, sizeof(described));
  if (!this->raw_queue_.push_back(operation)) {
    if (this->raw_queue_.capacity() == 0) {
      ESP_LOGW(TAG,
               "raw lane not allocated; dropping %s. Set raw_queue_size on the vitohome hub to use the scan console.",
               described);
    } else {
      ESP_LOGW(TAG, "raw queue full (%zu/%zu); dropping %s. Raise raw_queue_size for larger sweeps.",
               this->raw_queue_.size(), this->raw_queue_.capacity(), described);
    }

    return false;
  }

  ESP_LOGD(TAG, "Queued raw %s", described);
  return true;
}

void VitoHomeComponent::raw_handle_response_(const ResponseView &response) {
  // "0xXXXX:" (7) + RAW_READ_MAX * 3 hex chars + the integer views for widths
  // 1..8 + NUL fit in 208; the label and the time go in front.
  char buffer[240];
  int offset = 0;
  const uint32_t elapsed = millis() - this->in_flight_started_ms_;

  if (this->raw_layer_ != RawLayer::NONE) {
    char label[24];
    raw_label_(label, sizeof(label), this->raw_is_write_, this->raw_space_, this->raw_layer_, this->raw_code_,
               this->raw_frame_);
    offset = snprintf(buffer, sizeof(buffer), "%s, %" PRIu32 " ms: ", label, elapsed);
    if (offset < 0 || offset >= static_cast<int>(sizeof(buffer)))
      offset = 0;
  }
  if (this->raw_is_write_ && this->raw_layer_ == RawLayer::NONE) {
    const int written = snprintf(buffer, sizeof(buffer), "0x%04X: write ACK (%u byte%s)", this->raw_dp_.address(),
                                 this->raw_write_len_, this->raw_write_len_ == 1 ? "" : "s");
    // KW and GWG acknowledge a write with one byte; P300's acknowledgement
    // has none. KW's is 0x00, checked by the engine. GWG's is unconfirmed and
    // the engine takes any byte, so it is shown.
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
  raw_label_(label, sizeof(label), this->raw_is_write_, this->raw_space_, this->raw_layer_, this->raw_code_,
             this->raw_frame_);

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
  // publish_state(const char *) reuses each sensor's state string; a
  // std::string argument would allocate per call.
  for (uint16_t i = 0; i < this->raw_result_sensor_count_; i++) {
    if (this->raw_result_sensors_[i] != nullptr)
      this->raw_result_sensors_[i]->publish_state(line);
  }
#else
  (void) line;
#endif
}

void VitoHomeComponent::dispatch_raw_front_() {
  // Peek, hand over, remove: the operation stays at the head until the engine
  // accepts it, and the engine copies a write payload into its packet at once.
  // No lock is held across the hand-off, so a refusal is published from here
  // although a scan_result automation may queue the next operation. The head
  // cannot change in between: only this loop removes from the lane, and other
  // producers append.
  RawOp operation{};
  if (!this->raw_queue_.try_front(operation))
    return;

  this->raw_dp_ = optolink::Datapoint("scan", operation.address, operation.length, optolink::noconv);

  this->raw_is_write_ = operation.is_write;
  this->raw_space_ = operation.space;
  this->raw_layer_ = operation.layer;
  this->raw_code_ = operation.code;
  this->raw_frame_ = operation.frame;

#ifdef VITOHOME_RAW_SCRIPT
  if (operation.frame && raw_frame_timed(operation.waits, operation.now, operation.answer_p300)) {
    // The runner takes the port from an idle engine, which is not looped until
    // the script ends.
    if (this->vito_->isBusy())
      return;
    this->raw_queue_.drop_front();
    this->raw_write_len_ = 0;
    this->raw_script_start_(operation);
    this->set_in_flight_(OpType::RAW);
    return;
  }
#endif

  // A queue_raw_op() operation names its layer and code. Otherwise the space
  // picks the telegram (send_read_ / send_write_): a GWG access mode, a
  // procedure call with its parameter, a GFA read, or the protocol's own.
  const bool dispatched =
      operation.frame ? this->send_raw_frame_(operation.bytes, operation.bytes_len, operation.length)
      : operation.layer != RawLayer::NONE
          ? this->send_raw_op_(operation.address, operation.length, operation.layer, operation.code,
                               operation.is_write ? operation.bytes : nullptr, operation.bytes_len)
      : operation.is_write ? this->send_write_(operation.address, operation.bytes, operation.bytes_len, operation.space)
                           : this->send_read_(operation.address, operation.length, operation.space, operation.bytes,
                                              operation.bytes_len);
  char label[24];
  raw_label_(label, sizeof(label), operation.is_write, operation.space, operation.layer, operation.code,
             operation.frame);

  if (!dispatched) {
    // Busy is transient. A permanent refusal, such as a 16-bit address on GWG
    // (the GWG codec takes 0x00..0xFF), is dropped: kept at the head, it would stop
    // all bus traffic, since this lane goes first, with nothing in flight for
    // the watchdog.
    if (this->vito_->isBusy())
      return;
    char described[160];
    raw_describe_(operation, label, described, sizeof(described));
    ESP_LOGE(TAG, "Engine refused raw %s (permanent); dropping", described);
    this->raw_queue_.drop_front();
    this->raw_handle_error_(optolink::OptolinkResult::ERROR);
    return;
  }

  this->raw_queue_.drop_front();
  this->raw_write_len_ = operation.is_write ? operation.bytes_len : 0;
  // A KW telegram over P300 takes the GFA read's detour, so it counts as one
  // for the address-space affinity.
  this->last_space_ =
      operation.layer == RawLayer::KW && space_has_switch_cost(AddressSpace::GFA) ? AddressSpace::GFA : operation.space;
  // A GWG EEPROM, BE or PORT read, through its access mode or the generic
  // route's TYPE byte, takes one telegram per address.
  const bool gwg_per_address =
      operation.layer == RawLayer::GWG && !operation.frame && optolink::gwgReadTypeIsPerAddress(operation.code);
  this->set_in_flight_(OpType::RAW, nullptr,
                       operation.is_write || operation.frame ? 1
                       : gwg_per_address ? read_telegram_count(AddressSpace::GWG_EEPROM, operation.length)
                                         : read_telegram_count(operation.space, operation.length));

  ESP_LOGV(TAG, "Dispatched raw %s 0x%04X len %u", label, operation.address, operation.length);
}

}  // namespace esphome::vitohome
