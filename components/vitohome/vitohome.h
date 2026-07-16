#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "esphome/core/defines.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "optolink/optolink.h"
#include "protocol_select.h"
#include "response_view.h"
#include "ring_buffer.h"
#include "vito_clock.h"
#include "vito_entity.h"
#include "vito_uart_interface.h"

namespace esphome {

// Forward declaration only: the hub holds a pointer to a time source for the
// optional system-time sync. The real header is pulled into vitohome.cpp under
// VITOHOME_TIME_SYNC, so a build without a time: component (the common case)
// never needs the time component's sources on the include path.
namespace time {
class RealTimeClock;
}  // namespace time

namespace vitohome {

class VitoHomeComponent final : public PollingComponent, public uart::UARTDevice {
 public:
  // ESPHomeUARTInterface stores `this` as a UARTDevice*. Base subobjects have
  // already been constructed when member initialization begins, so passing
  // this pointer to iface_ here is valid. iface_ must not invoke virtual hub
  // behavior from its constructor.
  VitoHomeComponent() : iface_(this) {}

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Register one hub-owned entity pointer. Registration occurs during
  // code-generation setup, before VitoHomeComponent::setup() sizes the queues.
  //
  // Duplicate pointer registration is ignored defensively: the exact queue
  // sizing relies on the registered entity count being an upper bound for the
  // number of distinct entities that can be pending in a lane.
  void register_entity(VitoEntityBase *entity) {
    if (entity == nullptr)
      return;

    for (auto *registered : this->entities_) {
      if (registered == entity)
        return;
    }

    entity->set_vitohome_parent(this);
    this->entities_.push_back(entity);
  }

  // Force-refresh: mark every registered entity due on the next scheduler
  // tick by resetting its next_due_ms_ to the boot sentinel (0 = "never
  // polled -> due now"). The existing scheduler then does all the hard work:
  // read_queued_ dedup, queue backpressure, writes still preempt, read-backs
  // still jump the queue -- so a refresh is a self-throttling drain, the same
  // burst boot produces.
  //
  // Reads only; in-flight writes are untouched. If an entity already has a
  // read pending or in flight, the scheduler leaves it alone until that read
  // completes. Because next_due_ms_ remains at the sentinel, one additional
  // refresh read is scheduled afterward.
  //
  // Debounced by REFRESH_ALL_MIN_INTERVAL_MS so a misfiring Home Assistant
  // automation cannot pin the bus. Returns false when a call is suppressed.
  //
  // Callable from a lambda:
  //
  //   id(vito).refresh_all();
  bool refresh_all();

  // Connectivity binary sensors do not poll the bus themselves. The hub feeds
  // them its own view of the Optolink link, using device_class: connectivity in
  // Home Assistant.
  //
  // A complete response, a NACK, or a complete device ERROR frame
  // (OptolinkResult::DEVICE_ERROR) proves that the peer answered and resets
  // the timeout streak; malformed traffic (ERROR/CRC/LENGTH) proves nothing
  // and is ignored. OFFLINE is published after LINK_OFFLINE_AFTER_ERRORS
  // consecutive timeout/lost-callback observations, or when start-up protocol
  // verification fails. State is edge-published, so a healthy link does not
  // publish on every response.
#ifdef USE_BINARY_SENSOR
  void register_link_sensor(binary_sensor::BinarySensor *sensor) {
    if (sensor != nullptr)
      this->link_sensors_.push_back(sensor);
  }
#endif

  // device_id text sensors do not poll the bus themselves. They subscribe to
  // the hub's one-shot identification result.
#ifdef USE_TEXT_SENSOR
  void register_device_id_sensor(text_sensor::TextSensor *sensor) {
    if (sensor != nullptr)
      this->device_id_sensors_.push_back(sensor);
  }
#endif

  void set_identify_device(bool value) { this->identify_device_ = value; }

  // Capacity for the raw operation lane (the interactive scan console), applied
  // by setup()'s one-shot reserve(). Each slot costs sizeof(RawOp) (38 B on a
  // typical 32-bit ABI). RAW_QUEUE_DEFAULT is 0: the lane is a debug tool, so
  // only configs that actually scan pay for it, and an unallocated lane rejects
  // enqueues with a warning naming the option. Clock sync does NOT ride this
  // lane -- it is a VitoClock entity on the read/write lanes (vito_clock.h) --
  // so there is no minimum here.
  void set_raw_queue_capacity(std::size_t capacity) { this->raw_queue_capacity_ = capacity; }

  // System-time sync is optional. The hub periodically reads the device clock
  // at 0x088E and writes the configured time source back only when the measured
  // drift exceeds the configured threshold.
  //
  // The feature is inert unless a time source is assigned. The implementation
  // is a VitoClock (vito_clock.h) -- a hub-owned VitoEntityBase that rides the
  // ordinary read/write lanes -- and the whole thing is compiled behind
  // VITOHOME_TIME_SYNC so a configuration without a time component does not
  // pull that component into the build. These setters keep the same signatures
  // the codegen has always emitted and simply forward.
  void set_time_source(time::RealTimeClock *time_source) {
#ifdef VITOHOME_TIME_SYNC
    this->clock_.set_time_source(time_source);
#else
    (void) time_source;
#endif
  }

  void set_time_sync(uint32_t interval_ms, uint32_t drift_threshold_s, bool sync_on_boot) {
#ifdef VITOHOME_TIME_SYNC
    this->clock_.set_config(interval_ms, drift_threshold_s, sync_on_boot);
#else
    (void) interval_ms;
    (void) drift_threshold_s;
    (void) sync_on_boot;
#endif
  }

  // Queue a write for `entity`; the entity has already staged its payload in
  // its fixed write buffer.
  //
  // Writes are dispatched with priority over reads. If the entity is already
  // queued, the entity-owned payload is simply overwritten and the existing
  // queue item transmits the newest value. If a write is already in flight,
  // the entity may be queued once more so a newer value is sent afterward.
  //
  // Returns false when the entity/payload is invalid or the bounded write lane
  // rejected the item.
  //
  // Normal ESPHome control callbacks are loop-dispatched. RingBuffer protects
  // the queue itself, but does not independently synchronize concurrent writes
  // to the entity-owned staging buffer. External FreeRTOS tasks must defer
  // control operations to the ESPHome loop rather than mutate an entity
  // directly.
  bool request_write(VitoEntityBase *entity);

  // Queue a read for `entity` at the HEAD of the read lane, ahead of any
  // pending polls. Two callers, both of which need to jump the queue rather
  // than wait out a poll cycle (~150 s on a full catalog):
  //   * the write-ACK read-back, confirming the device's view of a value just
  //     written;
  //   * VitoClock's sync schedule, preserving the dispatch priority that clock
  //     sync had when it rode the raw lane.
  //
  // Returns true if the entity is queued OR was already queued (nothing to
  // add); false only if the bounded lane rejected the insertion, which the
  // read_queued_ dedup flag plus reserve(entities_.size()) make unreachable.
  bool request_priority_read(VitoEntityBase *entity);

  // --- raw scan console (debug) --------------------------------------------

  // Subscribe a hub-fed text sensor (text_sensor: type: scan_result) to raw
  // result lines. The sensor never polls.
#ifdef USE_TEXT_SENSOR
  void register_raw_result_sensor(text_sensor::TextSensor *sensor) {
    if (sensor != nullptr)
      this->raw_result_sensors_.push_back(sensor);
  }
#endif

  // Queue a one-off read or write to an arbitrary address.
  //
  // A SCAN item at the front of the shared raw FIFO is dispatched after
  // identification and before normal writes. CLOCK_* items are dispatched
  // after user writes. Because this is one FIFO, a scan item cannot overtake a
  // clock item already ahead of it.
  //
  // Results are logged and published to every scan_result text sensor.
  void queue_raw_read(uint16_t address, uint8_t length);
  void queue_raw_write(uint16_t address, const std::vector<uint8_t> &bytes);

 protected:
  enum class OpType : uint8_t {
    NONE,
    READ,
    WRITE,
  };

  // One-shot device identification, run immediately after protocol start-up:
  //
  //   step 0: read 0x00F8 length 4, covering F8..FB;
  //
  //   fallback: four length-1 reads at F8/F9/FA/FB. The length-1 reads at
  //             F8/F9 are wire-confirmed on the reference unit. Every fallback
  //             step is fail-soft.
  //
  // The result is logged, shown in dump_config(), and published to registered
  // device_id text sensors.
  enum class IdentState : uint8_t {
    IDLE,
    READ4,
    READ_F8,
    READ_F9,
    READ_FA,
    READ_FB,
    DONE,
  };

  void validate_uart_();
  void dispatch_next_();

  // Attempt to dispatch the current raw front item. The operation is removed
  // atomically only after the protocol engine accepted it.
  //
  // RingBuffer::consume_front_if() keeps the observed front stable across the
  // engine hand-off. The engine copies a write payload into its own packet
  // synchronously, so no RawOp reference escapes that operation.
  void dispatch_raw_front_();

  void schedule_due_entities_();

  void on_response_(const ResponseView &response, uint16_t request_address);
  void on_error_(optolink::OptolinkResult error, uint16_t request_address);

  // --- identification -------------------------------------------------------

  void ident_start_();
  void ident_dispatch_(IdentState state);
  void ident_handle_response_(const ResponseView &response);
  void ident_handle_error_();
  void ident_finish_();
  std::string ident_string_() const;

  // --- raw scan console -----------------------------------------------------
  //
  // This lane is the interactive debug console and nothing else. It used to
  // carry device-clock synchronization too, behind a RawPurpose tag; the clock
  // is now a VitoClock entity on the ordinary read/write lanes (vito_clock.h),
  // which removed the tag, the purpose-based arbitration, and the two failure
  // modes the sharing created.

  void raw_handle_response_(const ResponseView &response);
  void raw_handle_error_(optolink::OptolinkResult error);
  void raw_publish_(const std::string &line);

  // Shared enqueue for the raw lane. bytes/bytes_len is the write payload and
  // must be nullptr/0 for a read. The payload is copied into RawOp, so queue
  // storage remains self-contained and heap-free after setup.
  //
  // Returns false for invalid arguments or queue overflow.
  bool enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t *bytes, uint8_t bytes_len);

  bool ident_in_flight_{false};

 private:
  ESPHomeUARTInterface iface_;

  // Build-time-selected protocol engine. All three engines expose the same
  // byte-oriented read/write API and callback shape:
  //
  //   response(data, length, address)
  //   error(result, request_address)
  //
  // setup() wraps the raw response payload in ResponseView before forwarding it
  // to an entity or one of the hub's internal state machines.
  std::unique_ptr<optolink::OptolinkEngine<SelectedProtocol>> vito_;

  // True after any verified response from the selected protocol, including a
  // complete NACK/ERROR response that proves the peer spoke the protocol.
  bool link_established_{false};

  // Registration vectors are populated by code generation before setup().
  // Runtime queue sizing depends on entities_ being complete at setup time.
  std::vector<VitoEntityBase *> entities_;

#ifdef USE_TEXT_SENSOR
  std::vector<text_sensor::TextSensor *> device_id_sensors_;
#endif

#ifdef USE_BINARY_SENSOR
  std::vector<binary_sensor::BinarySensor *> link_sensors_;
#endif

  // Tri-state published connectivity:
  //   -1 = unknown / nothing published
  //    0 = offline
  //    1 = online
  int8_t link_state_{-1};

  uint8_t link_error_streak_{0};

  static constexpr uint8_t LINK_OFFLINE_AFTER_ERRORS = 3;

  uint32_t last_refresh_all_ms_{0};

  static constexpr uint32_t REFRESH_ALL_MIN_INTERVAL_MS = 5000;

  void publish_link_(bool up);

  // Record verified link activity. This resets the timeout streak, marks
  // start-up protocol verification successful, and edge-publishes ONLINE.
  void link_note_alive_();

  // Record one no-response/lost-callback observation. OFFLINE is edge-published
  // once the threshold is reached.
  void link_note_error_();

  // --- entity queues and in-flight state ------------------------------------
  //
  // Under normal operation each entity's read_queued_ / write_queued_ flag
  // prevents a second pending occurrence in the corresponding lane. The
  // registered entity count is therefore the expected maximum lane depth.
  //
  // read_queued_ covers both a queued read and a read currently in flight. It
  // is cleared by the response, error, mismatch, or watchdog completion path.
  //
  // write_queued_ covers only an item waiting in write_queue_. It is cleared
  // when the engine accepts that item. write_in_flight_ then covers the
  // transaction until ACK, error, mismatch, or watchdog completion. Keeping
  // them separate lets a newer value enqueue while an older write is in flight.
  //
  // Every insertion is checked. A rejected push rolls back its companion flag
  // instead of leaving the entity permanently marked as queued.
  //
  // RingBuffer synchronization is per object. Separate VitoHomeComponent
  // instances therefore own independent queues and locks. That does not make
  // it valid for multiple hubs to consume the same UART stream.
  RingBuffer<VitoEntityBase *> read_queue_;
  RingBuffer<VitoEntityBase *> write_queue_;

  VitoEntityBase *in_flight_{nullptr};
  OpType in_flight_op_{OpType::NONE};
  uint32_t in_flight_started_ms_{0};

  // --- identification state -------------------------------------------------

  bool identify_device_{true};
  IdentState ident_state_{IdentState::IDLE};

  optolink::Datapoint ident_dp_{"ident", 0x00F8, 4, optolink::noconv};

  int ident_group_{-1};
  int ident_controller_{-1};
  int ident_hw_{-1};
  int ident_sw_{-1};

  // --- raw scan console and clock lane --------------------------------------
  //
  // The raw lane contains complete inline operations and has exactly one item
  // in flight at a time. It carries both interactive scan-console operations
  // and background device-clock synchronization.
  //
  // Priority is determined by the FRONT item's purpose:
  //
  //   * SCAN at front: after identification, before user writes;
  //   * CLOCK_* at front: after user writes, before normal reads.
  //
  // This preserves FIFO order. A SCAN item behind CLOCK_* cannot overtake it.
  // True purpose-wide priority would require separate scan and clock queues.
  //
  // Write payloads are stored inline. Protocol engines serialize the payload
  // synchronously into their own packet buffer during write(), so queued bytes
  // do not need to outlive dispatch.
  static constexpr uint8_t RAW_WRITE_MAX = 32;

  // The read cap is separate and larger because a raw read stores no outbound
  // payload. Forty-eight bytes matches the widest catalog-generated text block
  // currently emitted and keeps format_raw_dump() output within its fixed
  // result buffer.
  static constexpr uint8_t RAW_READ_MAX = 48;

  struct RawOp {
    uint16_t address;
    uint8_t length;
    bool is_write;
    uint8_t bytes[RAW_WRITE_MAX];
    uint8_t bytes_len;
  };

  // The raw lane is explicitly bounded because a range sweep can enqueue much
  // faster than a 4800-baud link drains.
  //
  // RawOp carries RAW_WRITE_MAX inline bytes, so each slot is real RAM:
  // sizeof(RawOp) is 38 bytes on a typical 32-bit ABI, exact value dependent on
  // the selected compiler and target.
  //
  // DEFAULT 0: the lane is allocated only by configs that actually scan.
  //
  // This lane serves exactly one feature -- the interactive scan console
  // (queue_raw_read() / queue_raw_write() / the scan_result text_sensor). It is
  // a debug tool, so paying for it by default is backwards, and on a memory-
  // constrained target (ESP8266: ~40 KiB heap) a sweep-sized lane is a
  // quarter of the budget. Clock synchronization used to live here too and
  // forced a non-zero default; it is now a VitoClock entity on the ordinary
  // read/write lanes (vito_clock.h), so a config that does not scan needs no
  // raw lane at all.
  //
  // Scanning is therefore opt-in. Size it to the largest burst you intend:
  // a one-off queue_raw_read() from a button needs 1; a RANGE SWEEP needs
  // depth proportional to its count (example/vitohome-scanner-raw.yaml uses
  // 256, ~9.7 KiB). An enqueue against an unallocated lane is rejected with a
  // warning naming this option, so the failure is loud rather than silent.
  //
  // The size cannot be derived from the config: the lane is driven through
  // queue_raw_read()/queue_raw_write() from lambdas, and the shipped sweep's
  // count is a Home Assistant action parameter chosen at runtime -- the
  // required depth does not exist at codegen time in any form.
  //
  // This is the DEFAULT, not a ceiling; the YAML raw_queue_size option
  // overrides it through set_raw_queue_capacity() and is validated to
  // 0..1024 in __init__.py.
  static constexpr std::size_t RAW_QUEUE_DEFAULT = 0;
  std::size_t raw_queue_capacity_{RAW_QUEUE_DEFAULT};

  RingBuffer<RawOp> raw_queue_;

  bool raw_in_flight_{false};
  bool raw_is_write_{false};

  optolink::Datapoint raw_dp_{"scan", 0, 1, optolink::noconv};

  // Length of the last dispatched raw write. The payload itself is already in
  // the engine packet; only this length is retained for the ACK log line.
  uint8_t raw_write_len_{0};

#ifdef USE_TEXT_SENSOR
  std::vector<text_sensor::TextSensor *> raw_result_sensors_;
#endif

  // --- system-time synchronization ------------------------------------------

  // The clock is a hub-OWNED entity, not a codegen'd one: it has no platform
  // and no YAML of its own, so nothing else would construct it. setup()
  // register_entity()s it before sizing the lanes, so it counts toward
  // entities_.size() like any other participant.
  //
  // Under the #else there is no member at all -- a build without a time:
  // component carries none of this.
#ifdef VITOHOME_TIME_SYNC
  VitoClock clock_;
#endif

  // Failsafe for a lost engine callback. The protocol engines have their own
  // shorter transaction timeouts, but this prevents a permanently occupied hub
  // slot if a callback is ever lost because of an engine defect.
  static constexpr uint32_t IN_FLIGHT_WATCHDOG_MS = 10000;

  // Start-up protocol check. The selected engine must establish verified
  // protocol activity within max(PROTOCOL_VERIFY_MIN_MS, 3 * hub interval).
  //
  // The implementation caps the derived interval to INT32_MAX because the
  // rollover-safe signed-difference deadline comparison is valid only for
  // deadlines less than 2^31 milliseconds away.
  static constexpr uint32_t PROTOCOL_VERIFY_MIN_MS = 90000;

  bool protocol_verify_pending_{false};
  uint32_t protocol_verify_deadline_ms_{0};
};

}  // namespace vitohome
}  // namespace esphome
