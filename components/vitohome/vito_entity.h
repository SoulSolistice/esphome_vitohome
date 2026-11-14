#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "address_space.h"
#include "decode.h"
#include "optolink/optolink.h"
#include "response_view.h"

namespace esphome::vitohome {

class VitoHomeComponent;

// One {wire value -> label} row of a lookup table (text_sensor `options:` /
// `codes:`, event `codes:`). Codegen emits the tables as static const arrays
// in .rodata, so an entity holds a pointer and a count and setup() allocates
// nothing per row: 1883 rows across the catalogs would otherwise be 662 boot
// allocations, too many for an ESP8266's heap.
struct VitoOption {
  uint32_t value;
  const char *label;
};

// What only a writable entity carries (VitoWritableEntity).
struct VitoWritePath {
  // Distinct write target (command address); used only when has_datapoint.
  optolink::Datapoint datapoint{"uninitialized", 0, 1, optolink::noconv};
  bool has_datapoint{false};
  // Block mode: the field is block_width bytes at block_offset of the write
  // datapoint; 0 = not block mode.
  uint8_t block_offset{0};
  uint8_t block_width{0};
  bool read_back{true};
  uint8_t len{0};
  // The staged payload. 8 bytes: numbers and selects take up to 4, a day's
  // switching times and the device clock 8.
  uint8_t buf[8]{};
};

// An entity that owns an optolink datapoint. The hub polls it, hands it the
// answer or the error, and, for a writable entity, sends what it staged.
class VitoEntityBase {
  // The hub's queue bookkeeping (private section) is reachable by the hub
  // only.
  friend class VitoHomeComponent;

 public:
  virtual ~VitoEntityBase() = default;

  void set_datapoint(const optolink::Datapoint &dp) { this->datapoint_ = dp; }
  const optolink::Datapoint &get_datapoint() const { return this->datapoint_; }

  void set_vitohome_parent(VitoHomeComponent *parent) { this->vh_parent_ = parent; }

  // Where the datapoint lives, and so which telegram reads it (and writes it,
  // on a writable platform): the YAML `access:` (address_space.h). One space
  // drives both directions: Vitosoft pairs every writable GWG datapoint as
  // EEPROM_READ/EEPROM_WRITE or BE_READ/BE_WRITE, never across modes. OWN
  // until codegen sets another.
  AddressSpace space() const { return this->space_; }

  // A GWG access mode (meaningless, and OWN, outside protocol: GWG).
  void set_access(optolink::GWGAccessMode access) { this->space_ = space_from_gwg(access); }

  // `access: rpc` (P300 only): read as a remote procedure call, function code
  // 0x07 with `prefix:` as its parameter (the catalog's PrefixRead, typically
  // the index of an entry in a list behind one address, so several entities
  // share an address). The answer decodes like a read of datapoint_'s length.
  // Read-only platforms and the action button only (codegen refuses others).
  static constexpr uint8_t RPC_PREFIX_MAX = 4;
  void set_rpc_prefix(const std::vector<uint8_t> &prefix) {
    this->space_ = AddressSpace::RPC;
    this->rpc_prefix_len_ = static_cast<uint8_t>(std::min<std::size_t>(prefix.size(), RPC_PREFIX_MAX));
    if (this->rpc_prefix_len_ != 0)
      std::memcpy(this->rpc_prefix_, prefix.data(), this->rpc_prefix_len_);
  }
  bool is_rpc() const { return this->space_ == AddressSpace::RPC; }

  // Whether the telegram that reads this entity reads `other` too: same space,
  // address and length, and for a procedure call the same parameter. The hub
  // then sends one and hands the answer to both (vitohome.cpp,
  // share_read_response_). What each takes from the answer does not matter.
  bool reads_same_datapoint(const VitoEntityBase &other) const {
    if (this->space_ != other.space_ || this->datapoint_.address() != other.datapoint_.address() ||
        this->datapoint_.length() != other.datapoint_.length())
      return false;
    if (this->space_ != AddressSpace::RPC)
      return true;
    return this->rpc_prefix_len_ == other.rpc_prefix_len_ &&
           std::memcmp(this->rpc_prefix_, other.rpc_prefix_, this->rpc_prefix_len_) == 0;
  }
  const uint8_t *rpc_prefix() const { return this->rpc_prefix_; }
  uint8_t rpc_prefix_len() const { return this->rpc_prefix_len_; }

  // `access: gfa` (KW and P300): the burner control's address space, read with
  // the KW telegram 0x6B (Vitosoft's GFA_READ). The same address in the
  // Vitotronic is another datapoint. Read-only platforms only: Vitosoft
  // defines no GFA write.
  void set_gfa() { this->space_ = AddressSpace::GFA; }
  bool is_gfa() const { return this->space_ == AddressSpace::GFA; }

  // --- scheduling -----------------------------------------------------------
  // 0 (default): every hub cycle. Otherwise a minimum period, at the hub's
  // update_interval granularity.
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  uint32_t poll_interval() const { return this->poll_interval_ms_; }

  // Whether the poll scheduler queues this entity. False only for VitoClock,
  // which reads on its own sync schedule. Either way the entity counts for the
  // lane sizing and gets its answers and errors.
  virtual bool wants_polling() const { return true; }

  // --- read path ------------------------------------------------------------
  // A read answer; the engine has checked length and checksum.
  virtual void handle_response(const ResponseView &response) = 0;

  // A failed read (poll or read-back). Failed writes go to
  // handle_write_error(), so an entity can keep its state on a failed write
  // and still count failed reads.
  virtual void handle_error(optolink::OptolinkResult error) = 0;

  // A failed write. Default: nothing; the hub logs it, and the published state
  // still matches the device.
  virtual void handle_write_error(optolink::OptolinkResult /*error*/) {}

  // --- field extraction (`byte_offset`, `byte_length`) -----------------------
  // The datapoint is the block read from the wire, at the block base: an
  // interior read is answered with an error telegram on P300 and with 0xFF
  // fill on KW. The field is extract_len_ bytes at extract_byte_ inside it;
  // extract_byte_ < 0 makes the whole datapoint the field.
  void set_extract_byte(int16_t byte) { this->extract_byte_ = byte; }
  void set_extract_len(uint8_t len) { this->extract_len_ = len; }

  // --- bit field (`bit_mask`) -----------------------------------------------
  // Non-zero: the value is the masked bits of one byte (decode.h, "bit
  // fields"). A write stages the field value, and the hub writes it as a
  // read-modify-write of that byte (rmw.cpp), so the other bits go back as
  // read. Named field_mask because VitoBinarySensor's `bit_mask_` means "any
  // masked bit set"; the YAML key is `bit_mask` everywhere.
  void set_field_mask(uint8_t mask) { this->field_mask_ = mask; }
  uint8_t field_mask() const { return this->field_mask_; }

  // --- write path -----------------------------------------------------------
  // nullptr on a read-only entity; the accessors below then describe one that
  // never writes.
  virtual const VitoWritePath *write_path() const { return nullptr; }

  // The write target: the command address when one is set, else datapoint_.
  const optolink::Datapoint &get_write_datapoint() const {
    const VitoWritePath *w = this->write_path();
    return w != nullptr && w->has_datapoint ? w->datapoint : this->datapoint_;
  }
  uint8_t block_field_offset() const {
    const VitoWritePath *w = this->write_path();
    return w != nullptr ? w->block_offset : 0;
  }
  uint8_t block_field_width() const {
    const VitoWritePath *w = this->write_path();
    return w != nullptr ? w->block_width : 0;
  }
  // A bit field, or a field smaller than the block it is written in.
  bool needs_rmw() const { return this->field_mask_ != 0 || this->block_field_width() != 0; }
  // The bytes a write stages: the field in block mode, else the whole write
  // datapoint.
  uint8_t staged_width() const {
    const uint8_t width = this->block_field_width();
    return width != 0 ? width : this->get_write_datapoint().length();
  }
  const uint8_t *write_data() const {
    const VitoWritePath *w = this->write_path();
    return w != nullptr ? w->buf : nullptr;
  }
  uint8_t write_length() const {
    const VitoWritePath *w = this->write_path();
    return w != nullptr ? w->len : 0;
  }
  // Whether an acknowledged write is followed by a read of the device's value.
  bool wants_read_back() const {
    const VitoWritePath *w = this->write_path();
    return w != nullptr && w->read_back;
  }

  // The device acknowledged a write. Default: nothing (the hub queues the
  // read-back when wants_read_back()).
  virtual void handle_write_response(const ResponseView & /*response*/) {}

  // --- logging / dump_config --------------------------------------------------
  virtual const char *entity_kind() const = 0;

  // Also declared by ESPHome's Component, so one `override` in each concrete
  // entity covers both.
  virtual void dump_config() = 0;

 protected:
  // Failed reads (an error or an undecodable answer) in a row before an entity
  // reports "unknown". Writes never count: a failed write leaves the device
  // value as it was.
  static constexpr uint8_t UNKNOWN_AFTER_READ_FAILURES = 3;
  // Counts one failed read. True exactly once: on the failure that reaches the
  // threshold, when the caller publishes "unknown".
  bool count_read_failure_() {
    if (this->read_failures_ >= UNKNOWN_AFTER_READ_FAILURES)
      return false;
    return ++this->read_failures_ == UNKNOWN_AFTER_READ_FAILURES;
  }
  void clear_read_failures_() { this->read_failures_ = 0; }

  uint8_t field_width_() const { return this->extract_byte_ >= 0 ? this->extract_len_ : this->datapoint_.length(); }
  // Bytes a response must hold to contain the field.
  unsigned field_end_() const {
    return static_cast<unsigned>(this->extract_byte_ >= 0 ? this->extract_byte_ : 0) + this->field_width_();
  }
  // The field's bytes, or its integer value (with bit_mask applied), in a read
  // response; false when the response is shorter than the field.
  bool field_bytes_(const ResponseView &response, const uint8_t **field) const {
    return field_slice(response.data, response.data_length, this->extract_byte_, this->field_width_(), field);
  }
  bool field_value_(const ResponseView &response, uint32_t *value) const {
    return extract_field(response.data, response.data_length, this->extract_byte_, this->field_width_(),
                         this->field_mask_, value);
  }

  // Set by codegen. The converter is always noconv: decode.h does the
  // decoding.
  optolink::Datapoint datapoint_{"uninitialized", 0, 1, optolink::noconv};
  VitoHomeComponent *vh_parent_{nullptr};
  uint32_t poll_interval_ms_{0};
  // int16_t: an offset reaches 254, and -1 means "no extraction".
  int16_t extract_byte_{-1};
  uint8_t extract_len_{1};
  uint8_t field_mask_{0};
  AddressSpace space_{AddressSpace::OWN};
  uint8_t rpc_prefix_len_{0};
  uint8_t rpc_prefix_[RPC_PREFIX_MAX]{0, 0, 0, 0};
  uint8_t read_failures_{0};

 private:
  // The hub's queue state, not entity state; a subclass touching it would
  // corrupt the lanes.
  bool read_queued_{false};
  // write_queued_: waiting in the hub's write_queue_. write_in_flight_: sent,
  // waiting for ACK or error. Independent, so a value changed while a write is
  // in flight queues again and goes out after it.
  bool write_queued_{false};
  bool write_in_flight_{false};
  // Poll backoff (poll_backoff_interval()): poll timeouts in a row with the
  // link otherwise alive, and the hub's answer count at the last of them.
  uint8_t timeout_streak_{0};
  uint32_t next_due_ms_{0};
  uint32_t answers_at_timeout_{0};
};

// An entity that writes: number, select, switch, text, the action button, the
// climate channels and the clock.
class VitoWritableEntity : public VitoEntityBase {
 public:
  const VitoWritePath *write_path() const final { return &this->write_path_; }

  // A distinct write target: polling, read-back and answer matching use
  // datapoint_ (the state address), writes go here (the command address).
  void set_write_datapoint(const optolink::Datapoint &dp) {
    this->write_path_.datapoint = dp;
    this->write_path_.has_datapoint = true;
  }

  // Block mode (`byte_offset` without `state_address`): the field is `width`
  // bytes at `offset` of the write datapoint, which is the whole block. The
  // device writes its whole datapoint (a 1-byte write into a 2-byte block
  // stored the telegram's checksum in byte 1; docs/design_notes.md SS5b), so
  // the hub reads the block, replaces the field (decode.h, merge_block_field)
  // and writes all of it back.
  void set_block_field(uint8_t offset, uint8_t width) {
    this->write_path_.block_offset = offset;
    this->write_path_.block_width = width;
  }

  void set_read_back(bool v) { this->write_path_.read_back = v; }

 protected:
  bool set_write_payload_(const uint8_t *data, uint8_t len) {
    if (data == nullptr || len == 0 || len > sizeof(this->write_path_.buf))
      return false;
    std::memcpy(this->write_path_.buf, data, len);
    this->write_path_.len = len;
    return true;
  }

  VitoWritePath write_path_;
};

}  // namespace esphome::vitohome
