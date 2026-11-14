#pragma once
#include "esphome/core/defines.h"

#ifdef VITOHOME_TIME_SYNC

#include "esphome/components/time/real_time_clock.h"
#include "decode.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Device-clock synchronization as an entity on the hub's read/write lanes, with
// no Home Assistant presence, platform or YAML of its own (the hub's time_id /
// time_sync options configure it). The chain maps onto the entity calls:
//
//   read device clock  -> request_priority_read() -> handle_response()
//   compare drift      -> handle_response()
//   write device clock -> set_write_payload_() + request_write()
//   verify             -> wants_read_back() -> handle_response() again
//
// It is not polled (wants_polling() is false): tick() queues its reads at the
// head of the read lane, so sync_on_boot does not wait out a poll cycle (about
// 150 s on a full catalog). Drift is measured against the time source when the
// answer arrives, so a late read is not a wrong one. Writes go before reads
// anyway.
class VitoClock final : public VitoWritableEntity {
 public:
  // NRF_Uhrzeit~0x088E: the Vitotronic-family clock and the schema default
  // (__init__.py, CONF_CLOCK_ADDRESS); set_config() overrides it.
  static constexpr uint16_t CLOCK_ADDRESS_DEFAULT = 0x088E;

  // Fixed, unlike the address: every DateTimeBCD variant in the Vitosoft data
  // is 8 bytes, the size of VitoWritePath::buf.
  static constexpr uint8_t CLOCK_LEN = 8;

  VitoClock();

  // Called from to_code(), before App.setup(), so the callback is in place
  // before a time component can deliver its first sync.
  void set_time_source(time::RealTimeClock *time_source) {
    this->time_source_ = time_source;
    time_source->add_on_time_sync_callback([this]() { this->source_synced_ = true; });
  }

  // The clock address differs by device: 0x088E on NRF/Vitotronic (the
  // default, hardware-confirmed on a Vitodens 300-W), 0x08E0 on the WPR
  // heat-pump controllers, both the 8-byte DateTimeBCD. The weekday base is
  // read off the device's clock (clock_weekday_base, decode.h). GWG has three
  // 1-byte registers instead and is refused at config time. Called from
  // to_code(), so the datapoint is rebuilt here.
  void set_config(uint32_t interval_ms, uint32_t drift_threshold_s, bool sync_on_boot, uint16_t clock_address) {
    this->interval_ms_ = interval_ms;
    this->drift_threshold_s_ = drift_threshold_s;
    this->sync_on_boot_ = sync_on_boot;
    this->set_clock_address(clock_address);
  }
  // time_sync.weekday_base: how the clock counts its weekday byte, when known
  // (decode.h WeekdayBase). Unset, it is read off the clock (resolve_weekday_base).
  void set_weekday_base(WeekdayBase base) {
    this->configured_base_ = base;
    this->configured_base_ok_ = true;
  }

  // Driven from VitoHomeComponent::update(), at the hub's update_interval.
  void tick(uint32_t now_ms);

  // --- VitoEntityBase --------------------------------------------------------
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  void handle_write_error(optolink::OptolinkResult error) override;
  void handle_write_response(const ResponseView &response) override;
  void dump_config() override;
  const char *entity_kind() const override { return "clock"; }

  // tick() drives the schedule instead (see above).
  bool wants_polling() const override { return false; }

 protected:
  // The step of the chain. The verify read-back arrives through the same
  // handle_response() as the first read; without the phase the compare would
  // run again and could write again, forever.
  enum class Phase : uint8_t {
    IDLE,       // no chain in flight
    READING,    // initial read dispatched; response drives the drift compare
    WRITING,    // clock write dispatched; awaiting the device ACK
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
  bool sync_on_boot_{true};         // sync once the source first syncs
  // The weekday convention: configured (set_weekday_base), or learned from a
  // read that decided it (a Sunday); see resolve_weekday_base().
  WeekdayBase configured_base_{WeekdayBase::SUNDAY_0};
  bool configured_base_ok_{false};
  WeekdayBase learned_base_{WeekdayBase::SUNDAY_0};
  bool learned_ok_{false};
  bool did_boot_{false};      // first-synced-time handling completed
  uint32_t next_sync_ms_{0};  // next periodic sync; not next_due_ms_, the poll scheduler's

  // Set by the time source's on_time_sync callback, never cleared. A valid
  // now() is not enough to write the boiler: is_valid() only checks year >=
  // 2019, and on ESP32 the system clock survives a software reset, so a stale
  // time is "valid" at boot before any sync (it shifted a boiler clock by
  // 43 h). Where SNTP cannot signal a sync (RP2040, LibreTiny) the callback
  // fires as soon as now() is valid, so there this adds nothing.
  bool source_synced_{false};

  Phase phase_{Phase::IDLE};
};

}  // namespace esphome::vitohome

#endif  // VITOHOME_TIME_SYNC
