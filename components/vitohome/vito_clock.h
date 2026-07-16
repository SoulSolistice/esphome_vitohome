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
  VitoClock();

  void set_time_source(time::RealTimeClock *time_source) { this->time_source_ = time_source; }

  void set_config(uint32_t interval_ms, uint32_t drift_threshold_s, bool sync_on_boot) {
    this->interval_ms_ = interval_ms;
    this->drift_threshold_s_ = drift_threshold_s;
    this->sync_on_boot_ = sync_on_boot;
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

  time::RealTimeClock *time_source_{nullptr};

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
