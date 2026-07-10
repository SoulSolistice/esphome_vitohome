#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "optolink/optolink.h"
#include "protocol_select.h"
#include "response_view.h"
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

class VitoHomeComponent : public PollingComponent, public uart::UARTDevice {
 public:
  // ESPHomeUARTInterface stores `this` (as UARTDevice*); valid in the
  // member init list because base subobjects are already constructed.
  VitoHomeComponent() : iface_(this) {}

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void register_entity(VitoEntityBase* entity) {
    if (entity == nullptr) return;
    entity->set_vitohome_parent(this);
    this->entities_.push_back(entity);
  }

  // Force-refresh: mark every registered entity due on the next scheduler
  // tick by resetting its next_due_ms_ to the boot sentinel (0 = "never
  // polled -> due now"). The EXISTING scheduler then does all the hard work:
  // read_queued_ dedup, queue backpressure, writes still preempt, read-backs
  // still jump the queue -- so a refresh is a self-throttling drain, the
  // same burst boot produces. Reads only; in-flight writes are untouched.
  // Debounced (REFRESH_ALL_MIN_INTERVAL_MS) so a misfiring HA automation
  // loop cannot pin the bus; returns false when a call was suppressed.
  // Callable from a lambda: id(vito).refresh_all();
  bool refresh_all();

  // Connectivity binary sensors don't poll the bus themselves — the hub feeds
  // them its own view of the Optolink link (device_class: connectivity in
  // HA), so automations can react to link loss natively instead of templating
  // over stale entity timestamps. ONLINE on any successful response; OFFLINE
  // when start-up protocol verification fails or after
  // LINK_OFFLINE_AFTER_ERRORS consecutive protocol errors (watchdog
  // expiries included). State is edge-published (no per-response spam).
  void register_link_sensor(binary_sensor::BinarySensor* bs) {
    if (bs != nullptr) this->link_sensors_.push_back(bs);
  }

  // device_id text sensors don't poll the bus themselves — they subscribe to
  // the hub's one-shot identification result (see identification below).
  void register_device_id_sensor(text_sensor::TextSensor* ts) {
    if (ts != nullptr) this->device_id_sensors_.push_back(ts);
  }

  void set_identify_device(bool v) { this->identify_device_ = v; }

  // System-time sync (optional). The hub periodically reads the device clock
  // (0x088E) and, when it differs from the configured time source by more than
  // the drift threshold, writes the current time back. All three knobs are
  // user-configured; sync is inert unless a time source is set.
  void set_time_source(time::RealTimeClock* t) { this->time_source_ = t; }
  void set_time_sync(uint32_t interval_ms, uint32_t drift_threshold_s, bool sync_on_boot) {
    this->time_sync_interval_ms_ = interval_ms;
    this->time_drift_threshold_s_ = drift_threshold_s;
    this->time_sync_on_boot_ = sync_on_boot;
  }

  // Queue a write for `entity` (payload already staged in the entity's write
  // buffer). Writes are dispatched with priority over reads; the newest
  // payload wins if the entity is re-controlled while still queued (the
  // entity owns the buffer). Returns false if the entity has no payload.
  bool request_write(VitoEntityBase* entity);

  // --- raw scan console (debug) --------------------------------------------
  // Subscribe a hub-fed text sensor (text_sensor: type: scan_result) to the
  // raw-op result line. Mirrors register_device_id_sensor: it never polls.
  void register_raw_result_sensor(text_sensor::TextSensor* ts) {
    if (ts != nullptr) this->raw_result_sensors_.push_back(ts);
  }

  // Queue a one-off read / write to an arbitrary address, dispatched ahead of
  // regular polling (just after identification). The result -- hex + 64-bit
  // integer views for a read, ACK / error otherwise -- is logged and published
  // to any scan_result sensor. Drives the scan console and HA range sweeps.
  void queue_raw_read(uint16_t address, uint8_t length);
  void queue_raw_write(uint16_t address, const std::vector<uint8_t>& bytes);

 protected:
  enum class OpType : uint8_t { NONE, READ, WRITE };

  // One-shot device identification, run right after the protocol handshake:
  //   step 0: read 0x00F8 length 4 (Identification + IdentificationExtension
  //           in one transaction — the layout Vitosoft itself matches on);
  //   fallback: four length-1 reads at 0xF8/0xF9/0xFA/0xFB (the length-1
  //           reads at F8/F9 are wire-confirmed on the reference unit),
  //           each one fail-soft.
  // Result is logged at INFO, shown in dump_config, and pushed to any
  // registered device_id text sensors.
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
  // Dispatches raw_queue_.front() (caller must ensure it is non-empty) and
  // pops it on successful hand-off to the engine. Shared by both raw-lane
  // call sites in dispatch_next_() -- see the priority split there.
  void dispatch_raw_front_();
  void schedule_due_entities_();
  void on_response_(const ResponseView& response, uint16_t request_address);
  void on_error_(optolink::OptolinkResult error, uint16_t request_address);

  // identification
  void ident_start_();
  void ident_dispatch_(IdentState state);
  void ident_handle_response_(const ResponseView& response);
  void ident_handle_error_();
  void ident_finish_();
  std::string ident_string_() const;

  // raw scan console (debug)
  // Purpose tag for raw-lane ops: SCAN is the debug console, the CLOCK_* values
  // route a system-time read/write/verify through the same lane. Defined here so
  // it precedes enqueue_raw_ and the RawOp struct below.
  enum class RawPurpose : uint8_t { SCAN, CLOCK_READ, CLOCK_WRITE, CLOCK_VERIFY };
  void raw_handle_response_(const ResponseView& response);
  void raw_handle_error_(optolink::OptolinkResult error);
  void raw_publish_(const std::string& line);
  // Shared enqueue for the raw lane; the scan console uses purpose SCAN, the
  // clock sync uses the CLOCK_* purposes. bytes/bytes_len is the write payload
  // (nullptr/0 for reads), copied into the queued op -- the lane is heap-free.
  void enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t* bytes, uint8_t bytes_len,
                    RawPurpose purpose);

  // system-time sync (rides the raw lane)
  void time_sync_tick_();
  void sync_system_time_();
  void clock_handle_read_(const ResponseView& response);
  void clock_handle_write_ack_();
  void clock_handle_verify_(const ResponseView& response);
  static constexpr uint16_t CLOCK_ADDRESS = 0x088E;  // getSystemTime / setSystemTime
  static constexpr uint8_t CLOCK_LEN = 8;

  bool ident_in_flight_{false};

 private:
  ESPHomeUARTInterface iface_;
  // The protocol engine, build-time-selected via protocol_select.h. All three
  // engines share one byte-mover API (read/write on address/length primitives,
  // callbacks delivering (data, length, address)), so the hub drives the
  // selected engine directly; setup() wraps each callback's raw payload in a
  // ResponseView for the entities.
  std::unique_ptr<optolink::OptolinkEngine<SelectedProtocol>> vito_;
  // True once the engine has produced at least one valid response since
  // begin(). A valid response means the device speaks the configured protocol,
  // so this is the start-up verification signal.
  bool link_established_{false};

  std::vector<VitoEntityBase*> entities_;
  std::vector<text_sensor::TextSensor*> device_id_sensors_;
  std::vector<binary_sensor::BinarySensor*> link_sensors_;
  // tri-state: -1 unknown (nothing published yet), 0 offline, 1 online
  int8_t link_state_{-1};
  uint8_t link_error_streak_{0};
  static constexpr uint8_t LINK_OFFLINE_AFTER_ERRORS = 3;
  uint32_t last_refresh_all_ms_{0};
  static constexpr uint32_t REFRESH_ALL_MIN_INTERVAL_MS = 5000;
  void publish_link_(bool up);
  void link_note_error_();
  std::deque<VitoEntityBase*> read_queue_;
  std::deque<VitoEntityBase*> write_queue_;
  VitoEntityBase* in_flight_{nullptr};
  OpType in_flight_op_{OpType::NONE};
  uint32_t in_flight_started_ms_{0};

  // identification state
  bool identify_device_{true};
  IdentState ident_state_{IdentState::IDLE};
  optolink::Datapoint ident_dp_{"ident", 0x00F8, 4, optolink::noconv};
  int ident_group_{-1}, ident_controller_{-1}, ident_hw_{-1}, ident_sw_{-1};

  // Raw scan console (debug). A small FIFO of one-off ops; exactly one is in
  // flight at a time (raw_in_flight_), so bus arbitration stays single-owner
  // like the ident lane. The same lane carries the system-time sync ops,
  // tagged by RawPurpose (declared above) so the result is routed to the
  // clock logic instead of the scan console.
  //
  // Priority is split by purpose in dispatch_next_(), not uniform for the
  // whole lane: RawPurpose::SCAN is dispatched just below identification and
  // above write_queue_/read_queue_, so scan-console sweeps stay interactive.
  // RawPurpose::CLOCK_READ/CLOCK_WRITE/CLOCK_VERIFY are dispatched below
  // write_queue_ instead -- clock sync is a background task nobody is
  // watching, and it can take up to three sequential round trips, so it must
  // not be able to stall a user-initiated write for that long.
  // Write payloads are stored inline (heap-free, no per-retry copy): the cap
  // in queue_raw_write is 32 bytes and the engines serialize the payload into
  // their own packet buffer synchronously inside write(), so nothing needs to
  // outlive the dispatch.
  static constexpr uint8_t RAW_WRITE_MAX = 32;
  // The READ cap is separate and larger: a raw read stores no payload, so the
  // only limits are the engines' packet-length arithmetic (VS2 length byte is
  // 0x05 + len, VS1 frame length is payload + 4 -- both safe well past 200) and
  // format_raw_dump()'s output buffer. 48 matches text_sensor.py's
  // MAX_TEXT_BLOCK_LENGTH, the widest block the catalogs emit (the 42-byte
  // Beschriftung_* label blocks). Sized so the console can actually TEST the
  // reads the generator produces: at 32 it could not, which is why the P300
  // read-length question stayed open for a session longer than it needed to.
  static constexpr uint8_t RAW_READ_MAX = 48;
  struct RawOp {
    uint16_t address;
    uint8_t length;
    bool is_write;
    uint8_t bytes[RAW_WRITE_MAX];
    uint8_t bytes_len;
    RawPurpose purpose;
  };
  static constexpr size_t RAW_QUEUE_MAX = 256;
  std::deque<RawOp> raw_queue_;
  bool raw_in_flight_{false};
  bool raw_is_write_{false};
  RawPurpose raw_purpose_{RawPurpose::SCAN};
  optolink::Datapoint raw_dp_{"scan", 0, 1, optolink::noconv};
  // Length of the last dispatched raw write, kept only for the ack log line
  // (the payload itself is copied into the engine's packet at dispatch).
  uint8_t raw_write_len_{0};
  std::vector<text_sensor::TextSensor*> raw_result_sensors_;

  // System-time sync state. time_source_ == nullptr means the feature is off.
  time::RealTimeClock* time_source_{nullptr};
  uint32_t time_sync_interval_ms_{0};    // 0 = no periodic sync
  uint32_t time_drift_threshold_s_{60};  // only write if drift exceeds this
  bool time_sync_on_boot_{true};         // sync once after time first valid
  bool time_sync_did_boot_{false};       // boot sync already done
  uint32_t time_sync_next_ms_{0};        // next periodic sync (millis)

  // Failsafe: if a request is in flight for longer than this, log and
  // clear it. the optolink engine has its own internal timeout (via OptolinkResult
  // ::TIMEOUT) but if a callback is somehow lost the queue stalls.
  static constexpr uint32_t IN_FLIGHT_WATCHDOG_MS = 10000;

  // Start-up protocol check: the configured protocol must establish a link
  // (the adapter sees a first valid response) within this window, else the
  // component is marked failed. This catches a wrong protocol / wiring fault /
  // offline device instead of polling silently forever. The effective window is
  // max(this, 3x the hub update interval).
  static constexpr uint32_t PROTOCOL_VERIFY_MIN_MS = 90000;
  bool protocol_verify_pending_{false};
  uint32_t protocol_verify_deadline_ms_{0};
};

}  // namespace vitohome
}  // namespace esphome
