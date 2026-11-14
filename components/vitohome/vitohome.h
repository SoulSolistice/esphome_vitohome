#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"  // FixedVector

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "address_space.h"
#include "optolink/optolink.h"
#include "protocol_select.h"
#include "raw_op.h"
#include "response_view.h"
#include "ring_buffer.h"
#include "vito_clock.h"
#include "vito_entity.h"
#include "vito_uart_interface.h"

namespace esphome {

// The clock sync's time source. Only declared here; vitohome.cpp includes the
// header under VITOHOME_TIME_SYNC, so a build without a time: component needs
// none of its sources.
namespace time {
class RealTimeClock;
}  // namespace time

namespace vitohome {

class VitoHomeComponent final : public PollingComponent, public uart::UARTDevice {
 public:
  // iface_ keeps `this` as a UARTDevice*, a base constructed before the
  // members. Its constructor must not call into the hub.
  VitoHomeComponent() : iface_(this) {}

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Size the entity registry once, before the first register_entity(), to
  // codegen's upper bound on the registrations (__init__.py,
  // _entity_capacity_upper_bound). The hub's to_code() emits it ahead of every
  // platform's register_entity().
  void reserve_entities(std::size_t capacity);
  // Codegen registers each platform entity, setup() the hub's clock, both
  // before setup() sizes the lanes from the count. A duplicate is ignored. A
  // registration after that is refused with an error: the entity is never
  // scheduled, instead of a lane one slot short refusing some other entity
  // every cycle.
  void register_entity(VitoEntityBase *entity);

  // Mark every polled entity due now (next_due_ms_ = 0, "never polled"). The
  // scheduler drains them as at boot: no read is queued twice, writes still go
  // first, read-backs still jump the queue. Reads only. An entity with a read
  // pending or in flight keeps it and reads once more afterwards. At most once
  // per REFRESH_ALL_MIN_INTERVAL_MS, so a looping automation cannot hold the
  // bus; false when a call is dropped for that.
  //
  //   id(vito).refresh_all();
  bool refresh_all();

  // Connectivity binary sensors do not poll; the hub publishes its view of the
  // link to them, on change only.
  //
  // A complete response, a NACK or a device error frame (DEVICE_ERROR) is an
  // answer and resets the timeout streak; corrupt traffic (ERROR, CRC, LENGTH)
  // is ignored. OFFLINE follows LINK_OFFLINE_AFTER_ERRORS timeouts or lost
  // callbacks in a row, or a failed start-up check. Under GWG a timeout counts
  // only when the request never went out for want of an ENQ: a request the
  // unit took and left unanswered is an address it does not serve.
#ifdef USE_BINARY_SENSOR
  // One static table from codegen (__init__.py, _emit_hub_sensor_tables).
  // Const pointers to sensors that get published.
  void set_link_sensors(binary_sensor::BinarySensor *const *sensors, uint16_t count) {
    this->link_sensors_ = sensors;
    this->link_sensor_count_ = count;
  }
#endif

  // device_id text sensors do not poll; the hub publishes the identification.
#ifdef USE_TEXT_SENSOR
  void set_device_id_sensors(text_sensor::TextSensor *const *sensors, uint16_t count) {
    this->device_id_sensors_ = sensors;
    this->device_id_sensor_count_ = count;
  }
#endif

  void set_identify_device(bool value) { this->identify_device_ = value; }

  // Slots of the raw lane (scan console), allocated in setup(); see
  // RAW_QUEUE_DEFAULT.
  void set_raw_queue_capacity(std::size_t capacity) { this->raw_queue_capacity_ = capacity; }

  // Optional clock sync: read the device clock (clock_address, 0x088E by
  // default) and write the time source's time when the drift exceeds the
  // threshold. Inert without a time source. VitoClock (vito_clock.h) does the
  // work on the ordinary lanes. Without VITOHOME_TIME_SYNC these setters are
  // no-ops, so the generated code is the same either way.
  void set_time_source(time::RealTimeClock *time_source) {
#ifdef VITOHOME_TIME_SYNC
    this->clock_.set_time_source(time_source);
#else
    (void) time_source;
#endif
  }

  // time_sync.weekday_base (0 sunday=0, 1 monday=0, 2 monday=1..sunday=7);
  // not called when the option is `auto`.
  void set_clock_weekday_base(uint8_t base) {
#ifdef VITOHOME_TIME_SYNC
    this->clock_.set_weekday_base(static_cast<WeekdayBase>(base));
#else
    (void) base;
#endif
  }
  void set_time_sync(uint32_t interval_ms, uint32_t drift_threshold_s, bool sync_on_boot, uint16_t clock_address) {
#ifdef VITOHOME_TIME_SYNC
    this->clock_.set_config(interval_ms, drift_threshold_s, sync_on_boot, clock_address);
#else
    (void) interval_ms;
    (void) drift_threshold_s;
    (void) sync_on_boot;
    (void) clock_address;
#endif
  }

  // Queue a write of the payload `entity` has staged. Writes go before reads.
  // Still queued: the queued write sends the newest payload. In flight: the
  // entity queues once more for the newer value. False for an invalid entity
  // or payload, or a full lane.
  //
  // Call from the ESPHome loop: the lane is locked, the entity's staging
  // buffer is not, so another task must defer to the loop.
  bool request_write(VitoEntityBase *entity);

  // Queue a read for `entity` at the head of the read lane, ahead of polls:
  // the read-back after a write ACK, and VitoClock's sync. Neither should wait
  // out a poll cycle (about 150 s on a full catalog). True when queued or
  // already queued; false only for a full lane, which the lane sizing rules
  // out.
  bool request_priority_read(VitoEntityBase *entity);

  // --- raw scan console (debug) --------------------------------------------

  // scan_result text sensors: they do not poll; the hub publishes each raw
  // result line.
#ifdef USE_TEXT_SENSOR
  void set_raw_result_sensors(text_sensor::TextSensor *const *sensors, uint16_t count) {
    this->raw_result_sensors_ = sensors;
    this->raw_result_sensor_count_ = count;
  }
#endif

  // GWG diagnostics for A/B tests from a lambda. Not YAML keys: a production
  // config has no reason to pick a wire format. Each applies from the next
  // telegram. Accepted on every build, so lambdas need no #ifdef; only the GWG
  // engine uses them. Background: design_notes.md, "Writes" and "Bursting".
  //
  // Write frame layout: EOT after the payload (default) or before it.
  void set_gwg_write_eot_before_payload(bool eot_first);
  bool gwg_write_eot_before_payload() const { return this->gwg_write_eot_before_payload_; }
  // Whether a burst-sent request omits the leading 0x01 (default: omits).
  void set_gwg_burst_omits_enq_ack(bool omit);
  bool gwg_burst_omits_enq_ack() const { return this->gwg_burst_omits_enq_ack_; }
  // Answer deadline, clamped to optolink::GWG_RESPONSE_TIMEOUT_{MIN,MAX}_MS
  // (50..2000) and logged; default 250 ms. For one-byte reads keep it under the
  // device's idle-ENQ gap (about 700 ms), or an idle ENQ reads as the value.
  //
  //   id(vito).set_gwg_response_timeout_ms(700);
  void set_gwg_response_timeout_ms(uint32_t ms);
  uint32_t gwg_response_timeout_ms() const { return this->gwg_response_timeout_ms_; }

  // Ms to wait after the device's ENQ before sending the request that answers
  // it, clamped to 0..optolink::GWG_ENQ_SEND_DELAY_MAX_MS (100) and logged;
  // default 0, send on the ENQ. Burst-sent requests do not wait. The DEBUG
  // `GWG timing:` and `GWG ... unanswered` lines state the measured gap.
  //
  //   id(vito).set_gwg_enq_send_delay_ms(30);
  void set_gwg_enq_send_delay_ms(uint32_t ms);
  uint32_t gwg_enq_send_delay_ms() const { return this->gwg_enq_send_delay_ms_; }

  // True once the GWG engine has stopped bursting
  // (GWGEngine::BURST_FAILURES_BEFORE_FALLBACK), until reboot. A burst A/B test
  // checks it first: after the fallback every request waits for a fresh ENQ
  // and proves nothing about the burst frame. Always false off GWG.
  bool gwg_burst_disabled() const { return this->gwg_burst_disabled_; }

  // Scan console: queue a one-off read or write of any address on the raw
  // lane, which goes after identification and before writes and polls. The
  // result is logged and published to every scan_result text sensor.
  //
  // `access` is used under `protocol: GWG` only (GWGAccessMode, constants.h)
  // and ignored elsewhere, so lambdas compile on every protocol.
  void queue_raw_read(uint16_t address, uint8_t length,
                      optolink::GWGAccessMode access = optolink::GWGAccessMode::PHYSICAL);
  // The pointer form takes a stack buffer without allocating; the vector form
  // takes a braced-init-list from a lambda.
  void queue_raw_write(uint16_t address, const uint8_t *data, std::size_t len,
                       optolink::GWGAccessMode access = optolink::GWGAccessMode::PHYSICAL);
  void queue_raw_write(uint16_t address, const std::vector<uint8_t> &bytes,
                       optolink::GWGAccessMode access = optolink::GWGAccessMode::PHYSICAL);
  // Remote procedure call (P300 function code 0x07) with `parameter` as its
  // payload; the answer is published like a raw read's. A procedure that
  // answers a parameter with an entry of a list is read this way; one without
  // a reading to give -- clearing a history, stepping a sequence -- RUNS, on
  // the controller, at once. 0..32 parameter bytes. Under KW and GWG, which
  // have no such telegram, it is refused with a warning.
  void queue_raw_rpc(uint16_t address, const uint8_t *parameter, std::size_t len);
  void queue_raw_rpc(uint16_t address, const std::vector<uint8_t> &parameter);
  // A read in the burner control's address space (KW type 0x6B, Vitosoft's
  // GFA_READ), published like a raw read. Read-only: there is no GFA write.
  // Under P300 and GWG, which have no such telegram here, it is refused with a
  // warning.
  void queue_raw_gfa_read(uint16_t address, uint8_t length);
  // A generic raw operation, for reverse engineering: one telegram of any
  // protocol layer with any function code / type byte, from a spec line
  // ("p300 r 05 1000 16"; the syntax is at the top of raw_op.h) or its
  // fields. Needs `raw_access: unrestricted` on the hub, and an UNSAFE one
  // (RawClass, raw_op.h) also `unsafe_raw_writes: true`. The layer must be
  // reachable: p300 on a P300 hub, kw on a KW hub and on a P300 hub (through
  // the KW detour), gwg on a GWG hub. Published like a raw read, with the
  // controller's error code when a P300 error frame answers. Refused with a
  // warning otherwise; returns whether it was queued.
  bool queue_raw_op(const char *spec);
  bool queue_raw_op(const std::string &spec) { return this->queue_raw_op(spec.c_str()); }
  // `length` is the bytes asked for, or the size of `payload` when one is
  // given.
  bool queue_raw_op(RawLayer layer, uint8_t code, uint16_t address, uint8_t length, const uint8_t *payload = nullptr);

 protected:
  // What has the bus: the one operation in flight, and whose it is. One value
  // answers "is the bus free" and "who gets the callback", so the two cannot
  // disagree.
  enum class OpType : uint8_t {
    NONE,  // nothing in flight
    // An entity operation: in_flight_ is the entity.
    READ,
    WRITE,
    // First half of a read-modify-write: the read of what the merged write
    // replaces. Part of the write (write_in_flight_ is set), so its failure is
    // a failed write. See rmw_pending_.
    RMW_READ,
    // The hub's own, with no entity.
    IDENT,
    RAW,
  };

  // Device identification, once after start-up (and again on link-up if it
  // failed):
  //
  //   0x00F8 length 4 (F8..FB), or four length-1 reads if that fails;
  //
  //   P300/KW: 0x7650 length 1, the burner-control chip (Vitosoft's
  //   GFA_Kennung), which selects the GFA fault texts
  //   (example/catalogs/<lang>/gfa/). GWG addresses one byte and has no such
  //   address;
  //
  //   P300/KW: 0x00F0 length 2, Vitosoft's "Erweiterte Geraetekennung (F0)",
  //   which tells apart a few variants sharing one software-index range. Kept
  //   raw (F0=0xXXYY, &f0=XXYY, register order); the catalog finder decides
  //   what it selects. A unit without it answers with an error frame (P300) or
  //   FF FF (KW).
  //
  // Every step fails soft. The result is logged, shown in dump_config() and
  // published to device_id text sensors.
  enum class IdentState : uint8_t {
    IDLE,
    READ4,
    READ_F8,
    READ_F9,
    READ_FA,
    READ_FB,
    READ_GFA,
    READ_F0,
    DONE,
  };

  void dispatch_next_();

  // Hand the head of a lane to the engine: peek, hand over, remove once
  // accepted. dispatch_write_front_() returns whether the write lane held
  // anything (sent, dropped, or waiting for the engine): no read goes ahead of
  // a waiting write.
  void dispatch_raw_front_();
  bool dispatch_write_front_();
  void dispatch_read_front_();

  void schedule_due_entities_();

  void on_response_(const ResponseView &response, uint16_t request_address);
  void on_error_(optolink::OptolinkResult error, uint16_t request_address);
  // Poll backoff bookkeeping (poll_backoff_interval() in poll_schedule.h): a
  // poll read of `entity` timed out, or it answered.
  void note_poll_timeout_(VitoEntityBase *entity);
  void note_poll_answer_(VitoEntityBase *entity);

  // --- bit-field read-modify-write (see rmw_pending_) ------------------------
  void rmw_handle_read_(VitoEntityBase *entity, const ResponseView &response);
  void rmw_dispatch_write_();
  // Fail the read-modify-write of `entity` without sending anything; `why` is
  // logged.
  void rmw_abort_(VitoEntityBase *entity, optolink::OptolinkResult error, const char *why);
  void rmw_verify_read_back_(VitoEntityBase *entity, const ResponseView &response);
  // After a write ACK, queue a read for every other polled entity on the
  // written register (a test output and its speed byte, the coding flags of
  // one byte, a setpoint and a switch), so they show the new state now, not at
  // their next poll.
  void refresh_siblings_(VitoEntityBase *writer);

  // --- read coalescing --------------------------------------------------------
  // Entities often read one datapoint and differ only in what they take from
  // it: eight coding flags of one byte, the fields of a block, a sensor and a
  // text_sensor on one register. In the generated catalogs 14,453 of 82,650
  // entities (17%) repeat another's read.
  //
  // When a poll read completes, every other entity waiting in the read lane
  // for the same telegram (VitoEntityBase::reads_same_datapoint()) leaves the
  // lane with the same outcome, answer or error. A datapoint that does not
  // answer costs its timeout once, not once per entity.
  //
  // Only waiting entities are served, so each entity's interval still decides
  // when it reads. The answer is as fresh as the one it would have fetched,
  // except when its own write is queued behind the read in flight; that
  // write's ACK reads it again (read-back or refresh_siblings_()).
  void share_read_response_(VitoEntityBase *reader, const ResponseView &response);
  void share_read_error_(VitoEntityBase *reader, optolink::OptolinkResult error);
  // The next entity waiting in the read lane for `reader`'s telegram, taken
  // out of the lane, or nullptr. `from` is where to resume the scan of
  // entities_; it is advanced past the returned entity.
  VitoEntityBase *take_waiting_on_same_read_(VitoEntityBase *reader, std::size_t &from);

  // --- identification -------------------------------------------------------
  void ident_start_();
  void ident_dispatch_(IdentState state);
  void ident_handle_response_(const ResponseView &response);
  void ident_handle_error_();
  void ident_after_sw_();
  void ident_finish_();
  std::string ident_string_() const;
  // Writes the catalog finder URL for the identification read so far. Returns
  // false, leaving the buffer untouched, when the device id is unknown.
  bool ident_finder_url_(char *buffer, size_t size) const;
  // URL (49) + "?id=XXXX&hw=XX&sw=XX" (20) + "&gfa=XX" (7) + "&f0=XXXX" (8) + NUL = 85.
  static constexpr size_t CATALOG_FINDER_URL_SIZE = 88;

  // --- raw scan console -----------------------------------------------------
  void raw_handle_response_(const ResponseView &response);
  void raw_handle_error_(optolink::OptolinkResult error);
  void raw_publish_(const char *line);

  // Every raw operation is queued here. bytes/bytes_len is the write payload or
  // the procedure parameter, nullptr/0 for a read, copied into the RawOp. False
  // for invalid arguments or a full lane.
  bool enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t *bytes, uint8_t bytes_len,
                    AddressSpace space = AddressSpace::OWN, RawLayer layer = RawLayer::NONE, uint8_t code = 0);

  // The telegram of a queue_raw_op() operation on the selected engine. False
  // like send_read_(): busy, or never (a layer the protocol cannot reach).
  bool send_raw_op_(uint16_t address, uint8_t length, RawLayer layer, uint8_t code, const uint8_t *payload,
                    uint8_t payload_len);
  // "read" / "gfa read" / "p300 05 read" ...: what the raw operation in
  // flight (or being queued) is, for log and result lines.
  static void raw_label_(char *out, std::size_t cap, bool is_write, AddressSpace space, RawLayer layer, uint8_t code);

  // The telegram for a read (or procedure call) or a write of `address` in
  // `space`. False when the engine does not take it: busy, or never (also for
  // a space the protocol lacks); the callers tell them apart with isBusy().
  bool send_read_(uint16_t address, uint8_t length, AddressSpace space, const uint8_t *parameter = nullptr,
                  uint8_t parameter_len = 0);
  bool send_write_(uint16_t address, const uint8_t *data, uint8_t length, AddressSpace space,
                   const uint8_t *parameter = nullptr, uint8_t parameter_len = 0);

 private:
  ESPHomeUARTInterface iface_;

  // Selected at build time; all three engines have the same read/write calls
  // and callbacks, response(data, length, address) and
  // error(result, request_address).
  std::unique_ptr<optolink::OptolinkEngine<SelectedProtocol>> vito_;

  // Set by the first answer of the selected protocol, a NACK or device error
  // frame included.
  bool link_established_{false};

  // One allocation (reserve_entities()), filled by register_entity() before
  // setup() sizes the lanes from it. FixedVector never reallocates.
  FixedVector<VitoEntityBase *> entities_;

  // Set when setup() sizes the lanes; register_entity() refuses from then on.
  bool lanes_sized_{false};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *const *device_id_sensors_{nullptr};
  uint16_t device_id_sensor_count_{0};
#endif

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *const *link_sensors_{nullptr};
  uint16_t link_sensor_count_{0};
#endif

  // Published connectivity: -1 nothing published, 0 offline, 1 online.
  int8_t link_state_{-1};

  uint8_t link_error_streak_{0};

  static constexpr uint8_t LINK_OFFLINE_AFTER_ERRORS = 3;

#ifdef VITOHOME_PROTOCOL_GWG
  // All-0xFF answers in a row (gwg_note_all_ff_()).
  uint8_t gwg_all_ff_streak_{0};
  bool gwg_all_ff_warned_{false};

  // Above the longest plausible run of datapoints that really read 0xFF, such
  // as a few unset enums.
  static constexpr uint8_t GWG_ALL_FF_WARN_AFTER = 8;
#endif

  uint32_t last_refresh_all_ms_{0};
  // Answers delivered, any lane. A poll timeout advances an entity's backoff
  // streak only if this moved since its previous one (note_poll_timeout_()).
  // Only compared for equality, so wrapping is harmless.
  uint32_t answers_total_{0};

  static constexpr uint32_t REFRESH_ALL_MIN_INTERVAL_MS = 5000;

  void publish_link_(bool up);

  // An answer: resets the timeout streak, passes the start-up check, publishes
  // ONLINE on change.
  void link_note_alive_();
  // A timeout or lost callback: OFFLINE at LINK_OFFLINE_AFTER_ERRORS in a row.
  void link_note_error_();
#ifdef VITOHOME_PROTOCOL_GWG
  void gwg_note_all_ff_(const uint8_t *data, uint8_t length);
#endif

  // --- entity queues and in-flight state ------------------------------------
  //
  // An entity is at most once in each lane (read_queued_, write_queued_), so
  // the entity count bounds the lane depth.
  //
  // read_queued_: read queued or in flight, cleared on answer, error, address
  // mismatch or watchdog. write_queued_: waiting in write_queue_, cleared when
  // the engine takes it; write_in_flight_ then lasts until ACK, error,
  // mismatch or watchdog. So a newer value can queue while the older one is in
  // flight. A refused push rolls its flag back.
  //
  // Each hub has its own lanes and locks; two hubs on one UART still do not
  // work.
  RingBuffer<VitoEntityBase *> read_queue_;
  RingBuffer<VitoEntityBase *> write_queue_;

  // The operation in flight and, for an entity operation, its entity. Written
  // only by set_in_flight_() and take_in_flight_().
  OpType in_flight_op_{OpType::NONE};
  VitoEntityBase *in_flight_{nullptr};
  uint32_t in_flight_started_ms_{0};
  // The watchdog budget of the operation in flight (in_flight_watchdog_ms()).
  uint32_t in_flight_watchdog_ms_{IN_FLIGHT_WATCHDOG_MS};
  // Pushes the gwg_* diagnostics to the engine (GWG builds; no-op before setup).
  void apply_gwg_diagnostics_();
  // Start the watchdog for an operation of `telegrams` telegrams.
  void arm_watchdog_(uint8_t telegrams = 1);
  // The engine took a telegram: record whose it is, start the watchdog.
  // `entity` for READ, WRITE and RMW_READ only.
  void set_in_flight_(OpType operation, VitoEntityBase *entity = nullptr, uint8_t telegrams = 1) {
    this->in_flight_op_ = operation;
    this->in_flight_ = entity;
    this->arm_watchdog_(telegrams);
  }
  // The operation in flight has ended (answer, error, watchdog): free the bus
  // and return what it was, with its entity in `entity`.
  OpType take_in_flight_(VitoEntityBase *&entity) {
    const OpType operation = this->in_flight_op_;
    entity = this->in_flight_;
    this->in_flight_op_ = OpType::NONE;
    this->in_flight_ = nullptr;
    return operation;
  }

  // --- read-modify-write (bit fields and whole blocks) ------------------------
  //
  // An entity with needs_rmw() (a field_mask(), or a field smaller than its
  // block) writes only its field, in two transactions: an RMW_READ of the
  // whole write datapoint (one byte, or the block), then a WRITE of all of it
  // with only the field replaced (decode.h, merge_block_field). The rest, such
  // as eight coding flags at GWG EEPROM 0x0005 or a Neptun test output and its
  // speed byte, goes back as read. The device writes its whole datapoint
  // (docs/design_notes.md SS5b).
  //
  // INVARIANT (not host-provable; docs/design_notes.md SS10a, invariant 3):
  //   between an RMW_READ answer and its merged write, dispatch_next_() sends
  //   nothing else (rmw_pending_ is checked first), and every RMW_READ
  //   completion path (answer, error, address mismatch, watchdog) arms
  //   rmw_pending_ or ends in rmw_abort_(). So two quick changes to sibling
  //   fields never interleave.
  //
  // A failed read writes nothing; a blind write is the corruption this
  // prevents. All state is inline (MAX_RMW_BLOCK, checked by config and at
  // runtime); nothing is allocated.
 public:
  // Mirrors MAX_RMW_BLOCK in __init__.py. P300 writes of 1, 2 and 8 bytes are
  // hardware-proven; 16 covers every writable block in the export (the 42-byte
  // label blocks are strings, not a read-modify-write).
  static constexpr uint8_t MAX_RMW_BLOCK = 16;

 private:
  VitoEntityBase *rmw_pending_{nullptr};  // armed: merged block ready to write
  uint8_t rmw_buf_[MAX_RMW_BLOCK]{};      // the merged block to transmit
  uint8_t rmw_len_{0};
  // The last merged write sent, for its read-back: every byte outside the
  // field, and every bit outside the mask in it, must read back as written. A
  // difference (a sibling changed between read and write, or the device did
  // not take the write as sent) is logged.
  VitoEntityBase *rmw_verify_entity_{nullptr};
  uint8_t rmw_verify_buf_[MAX_RMW_BLOCK]{};
  uint8_t rmw_verify_len_{0};

  // --- identification state -------------------------------------------------

  bool identify_device_{true};
  IdentState ident_state_{IdentState::IDLE};

  optolink::Datapoint ident_dp_{"ident", 0x00F8, 4, optolink::noconv};

  int ident_group_{-1};
  int ident_controller_{-1};
  int ident_hw_{-1};
  int ident_sw_{-1};
  // Burner-control chip at 0x7650 (READ_GFA); -1 = not read, absent, or a
  // unit without one (error frame on P300, 0x00/0xFF on KW).
  int ident_gfa_{-1};
  // The two bytes at 0x00F0/0x00F1 (READ_F0), 0xF0 byte high; -1 = not read or
  // absent (error frame on P300, FF FF on KW). Uninterpreted on the device.
  int ident_f0_{-1};
  bool gwg_burst_disabled_{false};  // see gwg_burst_disabled()
#ifdef VITOHOME_PROTOCOL_GWG
  // Keeps the main loop from idling while the GWG engine holds an ENQ for
  // its ENQ-to-send delay (see GWGEngine::isHoldingEnq()); otherwise the delay
  // would overrun by up to one loop interval.
  HighFrequencyLoopRequester gwg_hold_loop_;
#endif
  // GWG diagnostics as last set; pushed to the engine at setup and on change.
  bool gwg_write_eot_before_payload_{optolink::GWGEngine::DEFAULT_WRITE_EOT_BEFORE_PAYLOAD};
  bool gwg_burst_omits_enq_ack_{optolink::GWGEngine::DEFAULT_BURST_OMITS_ENQ_ACK};
  uint32_t gwg_response_timeout_ms_{optolink::GWGEngine::RESPONSE_TIMEOUT_MS};
  uint32_t gwg_enq_send_delay_ms_{0};

  // --- raw scan console lane -------------------------------------------------
  //
  // Operations with their payload inline, one in flight at a time. The engine
  // copies the payload into its packet when it takes the write.
  static constexpr uint8_t RAW_WRITE_MAX = 32;

  // A read stores no payload, so its cap is separate: the widest text block in
  // the catalogs, and what format_raw_dump()'s result buffer holds.
  static constexpr uint8_t RAW_READ_MAX = 48;

  struct RawOp {
    uint16_t address;
    uint8_t length;
    bool is_write;
    uint8_t bytes[RAW_WRITE_MAX];
    uint8_t bytes_len;
    // Which telegram (address_space.h): a GWG access mode, a procedure call
    // (`bytes`/`bytes_len` its parameter, `length` equal to bytes_len, 0
    // allowed), a GFA read.
    //
    // No default member initializers: RingBuffer static_asserts a trivially
    // default-constructible element. `RawOp operation{};` value-initializes
    // this to OWN (0).
    AddressSpace space;
    // A queue_raw_op() operation: its layer and function code / type byte
    // (`bytes` its payload when is_write). NONE (0) for every other one.
    RawLayer layer;
    uint8_t code;
  };

  // Default raw_queue_size (0..1024, __init__.py). Each slot is sizeof(RawOp),
  // about 40 bytes on a 32-bit target, and a sweep-sized lane is a quarter of
  // an ESP8266's heap, so only configs that scan allocate one. An operation
  // queued on no lane is refused with a warning naming the option.
  //
  // Size it to the largest burst: 1 for a button's queue_raw_read(), the
  // sweep's count for a range sweep (example/vitohome-scanner-raw.yaml: 256,
  // about 9.7 KiB). Codegen cannot derive it: lambdas fill the lane, and the
  // sweep's count is a Home Assistant action parameter.
  static constexpr std::size_t RAW_QUEUE_DEFAULT = 0;
  std::size_t raw_queue_capacity_{RAW_QUEUE_DEFAULT};

  RingBuffer<RawOp> raw_queue_;

  bool raw_is_write_{false};
  AddressSpace raw_space_{AddressSpace::OWN};
  RawLayer raw_layer_{RawLayer::NONE};
  uint8_t raw_code_{0};
  // The space of the last telegram dispatched; see the address-space affinity
  // in dispatch_next_().
  AddressSpace last_space_{AddressSpace::OWN};

  optolink::Datapoint raw_dp_{"scan", 0, 1, optolink::noconv};

  // Length of the raw write in flight, for the ACK line.
  uint8_t raw_write_len_{0};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *const *raw_result_sensors_{nullptr};
  uint16_t raw_result_sensor_count_{0};
#endif

  // --- system-time synchronization ------------------------------------------

  // Owned by the hub: the clock has no platform or YAML of its own. setup()
  // registers it before sizing the lanes. Absent without a time: component.
#ifdef VITOHOME_TIME_SYNC
  VitoClock clock_;
#endif

  // For a callback the engine lost; the engines time out sooner.
  static constexpr uint32_t IN_FLIGHT_WATCHDOG_MS = 10000;

  // Start-up check: no answer within max(PROTOCOL_VERIFY_MIN_MS, 3 * hub
  // interval) logs an error, publishes the link offline and sets the error
  // status, and polling goes on: the device may be off. The first answer
  // clears all three (link_note_alive_()). The window is capped at INT32_MAX
  // for the signed-difference deadline check.
  static constexpr uint32_t PROTOCOL_VERIFY_MIN_MS = 90000;

  bool protocol_verify_pending_{false};
  uint32_t protocol_verify_deadline_ms_{0};
};

}  // namespace vitohome
}  // namespace esphome
