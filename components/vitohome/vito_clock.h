#pragma once
#include "esphome/core/defines.h"

#ifdef VITOHOME_TIME_SYNC

#include "esphome/components/time/real_time_clock.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Device-clock synchronization, expressed as a VitoEntityBase.
//
// WHY THIS IS AN ENTITY AND NOT A LANE OF ITS OWN
// -----------------------------------------------
// VitoEntityBase is deliberately NOT an ESPHome entity -- it inherits from
// nothing, and the concrete types (VitoSensor, ...) multiply-inherit their Home
// Assistant surface separately. So "a participant in the hub's datapoint lanes"
// and "a thing the user sees in Home Assistant" are already independent ideas
// here. VitoClock is the first: a pure lane participant with no HA presence, no
// platform, and no YAML of its own (it is configured by the hub's time_id /
// time_sync options).
//
// The clock chain maps onto the base class exactly:
//
//   read device clock  -> request_priority_read() -> handle_response()
//   compare drift      -> handle_response()
//   write device clock -> set_write_payload_() + request_write()
//   verify             -> wants_read_back() -> handle_response() again
//
// and the payload fits with zero growth: VitoEntityBase::write_buf_ is 8 bytes
// and the device clock datapoint (0x088E) is exactly 8 bytes of BCD.
//
// WHAT THIS REPLACED
// ------------------
// Clock sync used to ride the raw scan-console lane behind a RawPurpose tag.
// That coupled two unrelated tenants in one queue and produced two failure
// modes that do not exist here:
//   * a sweep filling the raw lane could starve a mid-chain clock write;
//   * raw_queue_size: 0 silently disabled time sync (hence a validator rule).
// The read/write lanes are reserve()d to entities_.size() and the
// read_queued_/write_queued_ flags admit an entity at most once, so they
// provably cannot fill. The raw lane is now purely the scan console and
// defaults to 0.
//
// PRIORITY
// --------
// The old raw lane was dispatched AHEAD of the poll lane. An ordinary polled
// entity is not: it queues behind every pending poll, which at a full catalog
// (~700 entities at ~4-5 ops/s) is a ~150 s wait. Harmless for drift itself
// (drift is measured against the live time source at response time, so a late
// read is not a wrong read) but a visible regression for sync_on_boot. So the
// clock is excluded from the poll rotation (wants_polling() == false) and
// pushes to the HEAD of the read lane from its own schedule, preserving the
// original ordering. Writes already preempt reads, so the write step needs
// nothing special.
class VitoClock : public VitoEntityBase {
 public:
  // NRF_Uhrzeit~0x088E: the Vitotronic-family clock, and the schema default.
  // Overridable per device -- see set_config(). Kept as a named constant so the
  // C++ fallback and the Python default (__init__.py's CONF_CLOCK_ADDRESS) are
  // each a single literal rather than scattered magic numbers.
  static constexpr uint16_t CLOCK_ADDRESS_DEFAULT = 0x088E;

  // Fixed, unlike the address: every DateTimeBCD variant in the Vitosoft data
  // is 8 bytes, and 8 is exactly VitoEntityBase::write_buf_.
  static constexpr uint8_t CLOCK_LEN = 8;

  VitoClock();

  void set_time_source(time::RealTimeClock *time_source) { this->time_source_ = time_source; }

  // clock_address is NOT a constant across Viessmann devices: NRF/Vitotronic
  // uses 0x088E (the schema default, hardware-confirmed on a Vitodens 300-W),
  // while the WPR heat-pump controllers use 0x08E0 for the same 8-byte
  // DateTimeBCD shape. Both are 8 bytes, so only the address moves -- which is
  // why one option covers it. GWG is a different shape entirely (three 1-byte
  // registers) and is rejected at config time rather than mis-served here.
  //
  // Called from to_code() before setup(), so the datapoint is rebuilt here
  // rather than in the constructor.
  void set_config(uint32_t interval_ms, uint32_t drift_threshold_s, bool sync_on_boot, uint16_t clock_address) {
    this->interval_ms_ = interval_ms;
    this->drift_threshold_s_ = drift_threshold_s;
    this->sync_on_boot_ = sync_on_boot;
    this->set_clock_address(clock_address);
  }

  // Driven from VitoHomeComponent::update(), so the granularity is the hub's
  // update_interval -- the same tick the old time_sync_tick_() ran on.
  void tick(uint32_t now_ms);

  // --- VitoEntityBase --------------------------------------------------------
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  void handle_write_error(optolink::OptolinkResult error) override;
  void handle_write_response(const ResponseView &response) override;
  void dump_config() override;
  const char *entity_kind() const override { return "clock"; }

  // Not polled on a datapoint interval: tick() drives the schedule and pushes
  // to the head of the read lane. See the PRIORITY note above.
  bool wants_polling() const override { return false; }

 protected:
  // Which step of the chain a read response belongs to. A verify read-back
  // arrives through the same handle_response() as the initial read, so without
  // this the compare would run again and could write again, forever.
  //
  // This is not new state: it is RawPurpose::CLOCK_{READ,WRITE,VERIFY} moved
  // out of the shared raw lane and made local to the only thing that used it.
  enum class Phase : uint8_t {
    IDLE,       // no chain in flight
    READING,    // initial read dispatched; response drives the drift compare
    VERIFYING,  // write ACKed; the read-back confirms what the device stored
  };

  void handle_read_(const ResponseView &response);
  void handle_verify_(const ResponseView &response);
  void abort_(const char *why);

  void set_clock_address(uint16_t address);

  time::RealTimeClock *time_source_{nullptr};

  uint16_t clock_address_{CLOCK_ADDRESS_DEFAULT};

  uint32_t interval_ms_{0};         // 0 = no periodic sync
  uint32_t drift_threshold_s_{60};  // write only above this drift
  bool sync_on_boot_{true};         // sync once time first becomes valid
  bool did_boot_{false};            // first-valid-time handling completed
  uint32_t next_sync_ms_{0};        // next periodic deadline

  // Deliberately not next_due_ms_: that field belongs to the poll scheduler,
  // and refresh_all() resets it across every entity. The clock is not polled,
  // so it keeps its own deadline and is unaffected by a refresh_all().
  Phase phase_{Phase::IDLE};
};

}  // namespace esphome::vitohome

#endif  // VITOHOME_TIME_SYNC
