#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

#include "decode.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "poll_schedule.h"
#ifdef VITOHOME_TIME_SYNC
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome::vitohome {

static const char *const TAG = "vitohome";

// What a raw-lane operation is, for its log lines.
static const char *raw_kind(bool is_write, bool is_rpc) { return is_rpc ? "rpc" : (is_write ? "write" : "read"); }

// Known device families (Identification at 0xF8/0xF9). Deliberately small:
// it covers the units this project has seen on the wire; everything else is
// reported as raw hex, and the catalogue tooling (scripts/gen_catalog.py)
// does the authoritative matching against the Vitosoft data.
// `hw` is the hardware index (0xFA), or a negative value when it has not been
// read yet. It only matters for GWG: see below.
static const char *ident_family_name(uint16_t ident, int hw = -1) {
  switch (ident) {
    case 0x20CB:
      return "VScotHO1";
    case 0x2098:
      return "V200KW2";
    case 0x2094:
      return "V200KW1";
    case 0x2053:
      // GWG identification is NOT one family per ident. 0x2053 covers four
      // distinct GWG unit families, discriminated by the hardware index --
      // source-confirmed against the Vitosoft ecnDataPointType identification
      // rows, where Identification is 0x2053 for all of them and
      // IdentificationExtension's high byte separates them (0x01xx VBEM,
      // 0x02xx VBES, 0x08xx VWMS, 0x10xx VBT2; the low byte is the software
      // index). Reporting a bare "GWG_VBEM" here -- as this did, and as
      // vcontrold's enum comment still does -- is wrong for three families out
      // of four.
      switch (hw) {
        case 0x01:
          return "GWG_VBEM";
        case 0x02:
          return "GWG_VBES";
        case 0x08:
          return "GWG_VWMS";
        case 0x10:
          return "GWG_VBT2";
        default:
          return "GWG";
      }
    case 0x2054:
      return "GWG_BT2";
    default:
      return nullptr;
  }
}

// Burner-control chip names, as Vitosoft's own chip table gives them
// (eventvaluetype Kennung_FA_Chip~<n>: "24h - NEC"). The byte read at 0x7650
// (GFA_Kennung) selects the GFA fault texts
// (example/catalogs/<lang>/gfa/chip_<XX>.yaml); the name is only for the log.
// Hardware: VScotHO1_72 reads 0x24.
static const char *gfa_chip_name(int chip) {
  switch (chip) {
    case 0x20:
      return "GFA";
    case 0x21:
      return "SCOT";
    case 0x22:
      return "Dover";
    case 0x23:
      return "CES";
    case 0x24:
      return "NEC";
    case 0x25:
      return "ET";
    case 0x26:
      return "Scot eSTB";
    case 0x27:
      return "Vi";
    case 0x41:
      return "\u00d6FA";  // oil burner control
    case 0x42:
      return "\u00d6FA (Vi)";
    default:
      return nullptr;
  }
}

// Catalog finder (web/ in this repository, published with GitHub Pages). The
// hub appends the identification bytes as a query string so the page opens on
// the matching catalog. The page does the matching from the generated
// example/catalogs/resolve.json; the device itself never fetches it.
static const char *const CATALOG_FINDER_URL = "https://soulsolistice.github.io/esphome_vitohome/";
// URL (49) + "?id=XXXX&hw=XX&sw=XX" (20) + "&gfa=XX" (7) + "&f0=XXXX" (8) + NUL = 85.
static const size_t CATALOG_FINDER_URL_SIZE = 88;

void VitoHomeComponent::reserve_entities(std::size_t capacity) { this->entities_.init(capacity); }

void VitoHomeComponent::register_entity(VitoEntityBase *entity) {
  if (entity == nullptr)
    return;

  if (this->lanes_sized_) {
    ESP_LOGE(TAG, "register_entity() after the lanes were sized; entity ignored");
    return;
  }

  for (auto *registered : this->entities_) {
    if (registered == entity)
      return;
  }

  // reserve_entities() sized entities_ to a codegen upper bound on the
  // registration count, so this must never be full. If it is, the codegen count
  // undercounted -- FixedVector would silently drop the push_back and the entity
  // would never poll, so fail loudly at boot instead.
  if (this->entities_.full()) {
    ESP_LOGE(TAG, "entity registry full (capacity %zu); '%s' dropped -- codegen entity count is wrong",
             this->entities_.capacity(), entity->get_datapoint().name());
    this->mark_failed(LOG_STR("entity registry full"));
    return;
  }

  entity->set_vitohome_parent(this);
  this->entities_.push_back(entity);
}

void VitoHomeComponent::setup() {
  // 4800 8E2 is enforced at config time (FINAL_VALIDATE_SCHEMA in __init__.py),
  // not read back from the bus here: see the note there on usb_uart.

#ifdef VITOHOME_TIME_SYNC
  // The clock has no platform, so no codegen registers it -- the hub owns it
  // and must register it itself. This MUST happen before the lane sizing below:
  // entity_count feeds reserve(), and the clock occupies a read slot (and,
  // when drift is corrected, a write slot) like any other participant.
  //
  // Every platform entity has already registered by now: ESPHome emits those
  // register_entity() calls into the generated setup() body, which runs before
  // App.setup() reaches this component.
  this->register_entity(&this->clock_);
#endif

  // Size the run-loop queues once, now that registration is complete. Each
  // lane makes exactly one element-storage allocation here and never
  // reallocates:
  //
  //   * read/write lanes: registered entity count;
  //   * raw lane: configured scan-sweep cap.
  //
  // All failures are handled at boot rather than leaving a partially working
  // component. Separate checks identify the allocation that failed.
  //
  // entity_count is sampled HERE, after every register_entity() call, and not
  // one line earlier. Sampling it above the VITOHOME_TIME_SYNC block reserved
  // entities_.size() - 1 and cost the clock's slot: with the clock's priority
  // read occupying the lane, the last due entity of a full poll cycle was
  // rejected every boot -- hardware-observed on VScotHO1_72 (2026-07-16):
  //
  //   [C][vitohome:563]: Entities: 56
  //   [E][vitohome:537]: Poll cycle: read queue rejected 1 due entities
  //                      (size=55, capacity=55)
  //
  // lanes_sized_ turns the ordering requirement from a comment into an
  // enforced invariant: register_entity() rejects anything arriving after this
  // point rather than silently under-sizing the lanes again.
  const std::size_t entity_count = this->entities_.size();
  this->lanes_sized_ = true;

  if (!this->read_queue_.reserve(entity_count)) {
    ESP_LOGE(TAG, "failed to allocate read queue for %zu entities", entity_count);
    this->mark_failed(LOG_STR("read queue allocation failed"));
    return;
  }

  if (!this->write_queue_.reserve(entity_count)) {
    ESP_LOGE(TAG, "failed to allocate write queue for %zu entities", entity_count);
    this->mark_failed(LOG_STR("write queue allocation failed"));
    return;
  }

  if (!this->raw_queue_.reserve(this->raw_queue_capacity_)) {
    ESP_LOGE(TAG, "failed to allocate raw queue with capacity %zu", this->raw_queue_capacity_);
    this->mark_failed(LOG_STR("raw queue allocation failed"));
    return;
  }

  // The engine is build-time-selected (protocol_select.h) and deduces the
  // interface type, wrapping &iface_ in a GenericInterface internally.
  this->vito_ = std::make_unique<optolink::OptolinkEngine<SelectedProtocol>>(&this->iface_);

  // All three engines share one byte-mover callback shape, so the hub registers
  // directly with the engine and wraps the raw payload in a ResponseView here.
  //
  // On P300 `address` is the one echoed in the device's own response frame (a
  // real wire-level datum); on KW/GWG the engine echoes the retained request
  // address. The engine is strictly single-in-flight and the hub tracks its own
  // in-flight context (in_flight_ / ident_in_flight_ / raw_in_flight_), so the
  // callback carries only that address as a wire-level cross-check.
  this->vito_->onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
    // A complete valid response proves link liveness and validates the
    // configured protocol for start-up verification.
    this->link_note_alive_();
#ifdef VITOHOME_PROTOCOL_GWG
    this->gwg_note_all_ff_(data, length);
#endif
    this->on_response_(ResponseView{data, length, address}, address);
  });

  this->vito_->onError([this](optolink::OptolinkResult error, uint16_t request_address) {
    // Link health tracks a persistent no-response condition rather than every
    // failed operation:
    //
    //   * NACK means the device received the request and actively rejected it.
    //   * DEVICE_ERROR is a COMPLETE, checksum-valid error frame from the
    //     device -- proof the peer answered in this protocol.
    //   * TIMEOUT means no usable reply arrived within the engine watchdog.
    //   * ERROR is malformed traffic (an invalid frame after a start byte),
    //     possibly line noise; CRC/LENGTH likewise indicate corruption. None
    //     of these establishes either a healthy response or a silent link.
    //
    // NACK and DEVICE_ERROR therefore reset the timeout streak (and satisfy
    // start-up protocol verification), while only TIMEOUT advances it.
    switch (error) {
      case optolink::OptolinkResult::NACK:
      case optolink::OptolinkResult::DEVICE_ERROR:
        this->link_note_alive_();
        break;

      case optolink::OptolinkResult::TIMEOUT:
        this->link_note_error_();
        break;

      case optolink::OptolinkResult::ERROR:
      case optolink::OptolinkResult::CRC:
      case optolink::OptolinkResult::LENGTH:
      default:
        break;
    }

    this->on_error_(error, request_address);
  });

#ifdef VITOHOME_PROTOCOL_GWG
  // Diagnostic instrument (2026-08-24), GWG-only: per-successful-read timing,
  // fed by RESPONSE_TIMEOUT_MS's own hardware capture (see GWGEngine::
  // OnTimingCallback). Logged at DEBUG on the existing `vitohome` tag -- no
  // new log tag or YAML option needed; a config that already sets
  // `logger: logs: {vitohome: DEBUG}` (as the diagnostic example does) sees
  // it for free, and it costs nothing when that tag is left at its default.
  this->vito_->onTiming([this](uint32_t enq_age_ms, uint32_t send_to_response_ms, uint16_t address) {
    ESP_LOGD(TAG, "GWG timing: 0x%04X enq age <=%" PRIu32 " ms, send->response %" PRIu32 " ms", address, enq_age_ms,
             send_to_response_ms);
  });

  // Burst fallback (2026-10-10). WARNING, not DEBUG: this permanently caps the
  // poll rate at one datapoint per ENQ interval -- 749-2070 ms on the captures
  // so far -- for the rest of the session, and until now it happened in total
  // silence because the engine's own warning is compiled out of normal builds
  // (see GWGEngine::OnBurstFallbackCallback).
  this->vito_->onBurstFallback([this](uint8_t consecutive_failures) {
    this->gwg_burst_disabled_ = true;
    ESP_LOGW(TAG,
             "GWG burst disabled after %u unanswered burst-sent request(s); polling is now one "
             "telegram per ENQ for the rest of this session (burst frames %s the leading 0x01; "
             "see gwgBurstOmitsEnqAck).",
             static_cast<unsigned>(consecutive_failures), optolink::gwgBurstOmitsEnqAck ? "omit" : "carry");
  });
#endif

  if (!this->vito_->begin()) {
    ESP_LOGE(TAG, "optolink engine begin() failed");
    this->mark_failed(LOG_STR("optolink engine begin() failed"));
    return;
  }

  // Require the configured protocol to establish a link within the start-up
  // window: max(PROTOCOL_VERIFY_MIN_MS, 3 * hub update interval).
  //
  // The deadline comparison in loop() uses a signed uint32_t difference and is
  // therefore valid only for deadlines less than 2^31 milliseconds away.
  // Saturate the derived window at INT32_MAX instead of allowing either the
  // multiplication or the signed-difference assumption to overflow.
  // Read once and reuse below for the per-entity interval warning -- this used
  // to be sampled twice into two differently-named locals (`interval` and
  // `hub_interval`) holding the same value.
  const uint32_t hub_interval = this->get_update_interval();
  constexpr uint32_t MAX_SIGNED_DEADLINE_MS = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());

  uint32_t verify_window;
  if (hub_interval > MAX_SIGNED_DEADLINE_MS / 3u) {
    verify_window = MAX_SIGNED_DEADLINE_MS;
  } else {
    verify_window = hub_interval * 3u;
  }

  if (verify_window < PROTOCOL_VERIFY_MIN_MS)
    verify_window = PROTOCOL_VERIFY_MIN_MS;

  this->protocol_verify_pending_ = true;

  // A one-shot setup anchor, not a hot-loop read.
  // App.get_loop_component_start_time() exists to avoid repeated slow millis()
  // reads on hot paths; that benefit does not apply to one setup-time deadline.
  this->protocol_verify_deadline_ms_ = millis() + verify_window;

  // Per-entity intervals are scheduled at hub-tick granularity, so anything
  // shorter than the hub interval silently degrades to the hub interval.
  // Surface that at setup instead of letting the user chase phantom lag.
  for (auto *entity : this->entities_) {
    if (entity == nullptr)
      continue;

    if (entity->poll_interval() != 0 && entity->poll_interval() < hub_interval) {
      ESP_LOGW(TAG,
               "%s '%s': update_interval %" PRIu32 " ms is shorter than the hub's %" PRIu32
               " ms; effective rate is the hub interval",
               entity->entity_kind(), entity->get_datapoint().name(), entity->poll_interval(), hub_interval);
    }
  }

  if (this->identify_device_)
    this->ident_start_();

  ESP_LOGI(TAG, "ready, %zu entities registered", this->entities_.size());
}

void VitoHomeComponent::loop() {
  if (this->vito_ == nullptr || this->is_failed())
    return;

  this->vito_->loop();

  // Frame logging (compile-time; see vito_uart_interface.h). TX frames are
  // emitted from write(); this closes an RX frame once the bus goes quiet.
  // Compiles to nothing without -DVITOHOME_LOG_FRAMES.
  this->iface_.frame_tick();

  // Start-up protocol verification: confirm the configured protocol actually
  // established a link, or fail the component with a clear message.
  if (this->protocol_verify_pending_) {
    if (this->link_established_) {
      this->protocol_verify_pending_ = false;
      ESP_LOGI(TAG, "%s link established", PROTOCOL_NAME);
    } else if (static_cast<int32_t>(App.get_loop_component_start_time() - this->protocol_verify_deadline_ms_) >= 0) {
      // Rollover-safe signed-difference comparison. setup() caps the deadline
      // distance at INT32_MAX so this comparison remains valid.
      this->protocol_verify_pending_ = false;

      ESP_LOGE(TAG, "%s link not established; check wiring and that the device speaks this protocol", PROTOCOL_NAME);

      this->publish_link_(false);
      this->mark_failed(LOG_STR("no Optolink link established"));

      // Do not continue into watchdog handling or dispatch after failure.
      return;
    }
  }

  // Watchdog: if a request has been in flight too long, surface that and free
  // the hub slot. The protocol engines have their own shorter timeout, so this
  // is a last-resort guard for a lost callback rather than the normal timeout
  // mechanism.
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_) {
    const uint32_t now = App.get_loop_component_start_time();

    if (now - this->in_flight_started_ms_ > IN_FLIGHT_WATCHDOG_MS) {
      // A lost engine callback is a link-health signal too.
      this->link_note_error_();

      if (this->ident_in_flight_) {
        ESP_LOGW(TAG, "Identification read exceeded watchdog (%" PRIu32 " ms)", IN_FLIGHT_WATCHDOG_MS);

        this->ident_in_flight_ = false;
        this->ident_handle_error_();
      } else if (this->raw_in_flight_) {
        ESP_LOGW(TAG, "Raw %s 0x%04X exceeded watchdog (%" PRIu32 " ms)", this->raw_is_write_ ? "write" : "read",
                 this->raw_dp_.address(), IN_FLIGHT_WATCHDOG_MS);

        this->raw_in_flight_ = false;
        this->raw_handle_error_(optolink::OptolinkResult::TIMEOUT);
      } else {
        // in_flight_ is known non-null here because the identification and raw
        // branches were excluded.
        ESP_LOGW(TAG, "In-flight %s to %s exceeded watchdog (%" PRIu32 " ms). Clearing.",
                 this->in_flight_op_ == OpType::WRITE      ? "write"
                 : this->in_flight_op_ == OpType::RMW_READ ? "read-before-write"
                                                           : "read",
                 this->in_flight_->get_datapoint().name(), IN_FLIGHT_WATCHDOG_MS);

        VitoEntityBase *entity = this->in_flight_;
        const OpType operation = this->in_flight_op_;

        this->in_flight_ = nullptr;
        this->in_flight_op_ = OpType::NONE;

        if (operation == OpType::READ) {
          if (this->rmw_verify_entity_ == entity)
            this->rmw_verify_entity_ = nullptr;  // the read-back is lost; never verify a later poll
          entity->read_queued_ = false;
          entity->handle_error(optolink::OptolinkResult::TIMEOUT);
        } else if (operation == OpType::WRITE) {
          if (this->rmw_verify_entity_ == entity)
            this->rmw_verify_entity_ = nullptr;
          entity->write_in_flight_ = false;
          entity->handle_write_error(optolink::OptolinkResult::TIMEOUT);
        } else if (operation == OpType::RMW_READ) {
          this->rmw_abort_(entity, optolink::OptolinkResult::TIMEOUT, "read-before-write lost (watchdog)");
        } else {
          // Unreachable against this file as written: in_flight_ and
          // in_flight_op_ are set together (dispatch_write_/dispatch_read_) and
          // cleared together (here, on_response_, on_error_), so a non-null
          // in_flight_ always carries a READ or a WRITE. Kept as defence in
          // depth because the failure mode is silent and permanent, not noisy
          // and transient: a dispatched entity has already been popped from its
          // lane, and BOTH re-queue paths refuse to re-add it while its flag is
          // set (schedule_due_entities_ counts it as skipped; queue_read_
          // returns early as "already pending"). So a flag left set here would
          // strand that entity until reboot, with this one ERROR line as the
          // only trace.
          //
          // Clearing both flags is safe precisely because the state is
          // contradictory: whichever lane the entity was really in, its flag is
          // now false and the scheduler can re-queue it on the next due tick.
          // The cost of guessing wrong is one skipped poll; the cost of not
          // guessing is a dead entity. No handle_error() call, deliberately --
          // this is not a protocol timeout to report to the entity, it is an
          // internal bookkeeping fault, and the entity has no in-flight
          // operation to fail.
          ESP_LOGE(TAG, "watchdog found %s in flight with no operation type; clearing lane flags",
                   entity->get_datapoint().name());
          entity->read_queued_ = false;
          entity->write_in_flight_ = false;
        }
      }
    }
  }

  // Dispatch the next queued request if the bus is idle.
  this->dispatch_next_();
}

void VitoHomeComponent::dispatch_raw_front_() {
  // Keep the front item stable across the engine hand-off and remove it only
  // if read()/write() accepts it. The engine copies a write payload into its
  // own fixed packet synchronously inside write().
  this->raw_queue_.consume_front_if([this](const RawOp &operation) {
    this->raw_dp_ = optolink::Datapoint("scan", operation.address, operation.length, optolink::noconv);

    this->raw_is_write_ = operation.is_write;
    this->raw_is_rpc_ = operation.is_rpc;

    bool dispatched;
    if (operation.is_rpc) {
#ifdef VITOHOME_HAS_RPC
      dispatched = this->vito_->rpc(operation.address, operation.bytes, operation.bytes_len);
#else
      dispatched = false;  // unreachable: queue_raw_rpc() refuses it on this build
#endif
    } else if (operation.is_write) {
#ifdef VITOHOME_PROTOCOL_GWG
      // GWG-only: the scan console can now pick the write access mode too, so a
      // tester can exercise EEPROM (0xAD) / BE (0x9D) -- the only modes Vitosoft
      // marks writable -- rather than only PHYSICAL (0xC8).
      dispatched = this->vito_->write(this->raw_dp_.address(), operation.bytes, operation.bytes_len, operation.access);
#else
      dispatched = this->vito_->write(this->raw_dp_.address(), operation.bytes, operation.bytes_len);
#endif
    } else {
#ifdef VITOHOME_PROTOCOL_GWG
      // GWG-only: access mode selects the TYPE byte (see GWGAccessMode,
      // constants.h). operation.access defaults to PHYSICAL, so a scan
      // console user who never passes a third argument to queue_raw_read()
      // gets exactly the pre-existing behaviour.
      dispatched = this->vito_->read(this->raw_dp_.address(), this->raw_dp_.length(), operation.access);
#else
      dispatched = this->vito_->read(this->raw_dp_.address(), this->raw_dp_.length());
#endif
    }

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
        return false;  // transient: keep the item at the head and retry
      ESP_LOGE(TAG, "Engine refused raw %s 0x%04X len %u (permanent); dropping",
               raw_kind(operation.is_write, operation.is_rpc), operation.address, operation.length);
      this->raw_handle_error_(optolink::OptolinkResult::ERROR);
      return true;  // permanent: drop so the read/write lanes are reached
    }

    this->raw_write_len_ = operation.is_write ? operation.bytes_len : 0;
    this->raw_in_flight_ = true;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();

    ESP_LOGV(TAG, "Dispatched raw %s 0x%04X len %u", raw_kind(operation.is_write, operation.is_rpc), operation.address,
             operation.length);

    return true;
  });
}

void VitoHomeComponent::dispatch_next_() {
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_)
    return;

  // A bit-field write whose byte has just been read goes out before anything
  // else -- identification, the raw console, other writes, polls. That is the
  // whole atomicity argument (see rmw_pending_ in vitohome.h): nothing may
  // touch the bus between the read and the merged write.
  if (this->rmw_pending_ != nullptr) {
    this->rmw_dispatch_write_();
    return;
  }

  // Identification runs before regular traffic so the user sees the device
  // tuple in the first seconds of the log.
  if (this->ident_state_ != IdentState::IDLE && this->ident_state_ != IdentState::DONE) {
#ifdef VITOHOME_PROTOCOL_GWG
    // GWG identification lives in the VIRTUAL access space, not the physical
    // one (2026-08-24). Vitosoft's own GWG tables put the four `SystemIdent`
    // fields at 0xF8..0xFB under FCRead=Virtual_READ, and vcontrold agrees --
    // its getDevType reads 0xF8 len 4 with GETVADDR (0xC7). Reading them on
    // the physical mode returns whatever the physical register file holds at
    // those offsets, which is what produced the unstable identifier this
    // project logged on its GWG capture. Every other protocol has exactly one
    // read space and is unaffected.
    const bool ident_dispatched =
        this->vito_->read(this->ident_dp_.address(), this->ident_dp_.length(), optolink::GWGAccessMode::VIRTUAL);
#else
    const bool ident_dispatched = this->vito_->read(this->ident_dp_.address(), this->ident_dp_.length());
#endif
    if (ident_dispatched) {
      this->ident_in_flight_ = true;
      this->in_flight_started_ms_ = App.get_loop_component_start_time();

      ESP_LOGV(TAG, "Dispatched identification read 0x%04X len %u", this->ident_dp_.address(),
               this->ident_dp_.length());
    } else if (!this->vito_->isBusy()) {
      // Permanent createPacket() rejection (unreachable: the ident addresses
      // are 0x00F8..0x00FB, plus 0x7650 on P300/KW only). Without this, a
      // permanent refusal would spin here forever
      // before the raw/read/write lanes are reached, with nothing in flight for
      // the watchdog. Advance the ident state machine exactly as the watchdog
      // does on a lost callback, so identification degrades field-by-field.
      ESP_LOGE(TAG, "Engine refused identification read 0x%04X (permanent); skipping", this->ident_dp_.address());
      this->ident_handle_error_();
    }

    return;
  }

  // Interactive scan-console operations preempt regular polling and queued user
  // writes: the console is a human waiting at a button, and the lane is empty
  // unless someone is actively scanning.
  //
  // This used to be conditional on a purpose tag, because device-clock
  // synchronization shared this queue and had to keep FIFO order against SCAN
  // items. The clock is now a VitoClock entity on the read/write lanes
  // (vito_clock.h), so this lane has exactly one tenant and needs no
  // arbitration.
  if (!this->raw_queue_.empty()) {
    this->dispatch_raw_front_();
    return;
  }

  // Writes preempt reads. The ring keeps the entity stable at the front until
  // the engine accepts it.
  const auto write_result = this->write_queue_.consume_front_if([this](VitoEntityBase *entity) {
    if (entity == nullptr) {
      // A null entry violates registration/enqueue invariants. Remove it so it
      // cannot block all later writes.
      ESP_LOGE(TAG, "null entity in write queue; dropping item");
      return true;
    }

    if (entity->needs_rmw()) {
      // Read first (RMW_READ): the WHOLE write datapoint -- one byte for a plain
      // bit field, the block in block mode -- at the write address and in the
      // entity's own access mode, i.e. exactly what the merged write replaces.
      // Config validation makes the read and the write the same register (no
      // state_address with a read-modify-write).
      const optolink::Datapoint &wdp = entity->get_write_datapoint();
      if (wdp.length() == 0 || wdp.length() > MAX_RMW_BLOCK) {
        // Unreachable through validated config; fail closed rather than send a
        // partial block.
        ESP_LOGE(TAG, "%s: %u-byte read-modify-write exceeds %u bytes; dropping", entity->get_datapoint().name(),
                 wdp.length(), MAX_RMW_BLOCK);
        entity->write_queued_ = false;
        entity->handle_write_error(optolink::OptolinkResult::LENGTH);
        return true;
      }
#ifdef VITOHOME_PROTOCOL_GWG
      const bool read_ok = this->vito_->read(wdp.address(), wdp.length(), entity->access());
#else
      const bool read_ok = this->vito_->read(wdp.address(), wdp.length());
#endif
      if (!read_ok) {
        // Same transient/permanent split as the plain write below.
        if (this->vito_->isBusy())
          return false;
        ESP_LOGE(TAG, "Engine refused read-before-write for %s (permanent); dropping", entity->get_datapoint().name());
        entity->write_queued_ = false;
        return true;
      }
      this->in_flight_ = entity;
      this->in_flight_op_ = OpType::RMW_READ;
      this->in_flight_started_ms_ = App.get_loop_component_start_time();
      entity->write_queued_ = false;
      entity->write_in_flight_ = true;
      ESP_LOGV(TAG, "Dispatched read-before-write for %s (%u bytes, bit_mask 0x%02X)", entity->get_datapoint().name(),
               wdp.length(), entity->field_mask());
      return true;
    }

#ifdef VITOHOME_PROTOCOL_GWG
    // GWG-only: one access mode drives both directions (see
    // VitoEntityBase::access_), so a writable entity writes in the same mode it
    // polls in -- which is what Vitosoft's (EEPROM_READ, EEPROM_WRITE) /
    // (BE_READ, BE_WRITE) pairing says. Defaults to PHYSICAL, i.e. the single
    // 0xC8 this lane emitted before.
    const bool write_ok = this->vito_->write(entity->get_write_datapoint().address(), entity->write_data(),
                                             entity->write_length(), entity->access());
#elif defined(VITOHOME_HAS_RPC)
    // An action (VitoActionButton) is a procedure call, not a write: the same
    // telegram an `access: rpc` read sends, here for its effect.
    const bool write_ok =
        entity->is_rpc()
            ? this->vito_->rpc(entity->get_write_datapoint().address(), entity->rpc_prefix(), entity->rpc_prefix_len())
            : this->vito_->write(entity->get_write_datapoint().address(), entity->write_data(), entity->write_length());
#else
    const bool write_ok =
        this->vito_->write(entity->get_write_datapoint().address(), entity->write_data(), entity->write_length());
#endif
    if (!write_ok) {
      // Transient (_busy) versus a permanent createPacket() rejection. Config-
      // declared write addresses/payloads are schema-validated, so the
      // permanent case is unreachable today; this mirrors the raw-lane guard so
      // the write lane can never spin on a permanent refusal. Drop it, clear
      // write_queued_ so a fresh value can enqueue, and log loudly. No
      // handle_write_error() here: some entities (VitoClock) re-queue on write
      // error, which would re-add a permanently-refused item and churn.
      if (this->vito_->isBusy())
        return false;  // transient: retain at the head
      ESP_LOGE(TAG, "Engine refused write for %s (permanent); dropping", entity->get_datapoint().name());
      entity->write_queued_ = false;
      return true;  // permanent: drop so the read lane is reached
    }

    this->in_flight_ = entity;
    this->in_flight_op_ = OpType::WRITE;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();

    // The entity has left the queue and is now in flight. Clearing
    // write_queued_ here lets a newer value enqueue while this request waits
    // for its ACK.
    entity->write_queued_ = false;
    entity->write_in_flight_ = true;

    ESP_LOGV(TAG, "Dispatched write for %s (%u bytes)", entity->get_datapoint().name(), entity->write_length());

    return true;
  });

  if (write_result != RingBuffer<VitoEntityBase *>::ConsumeResult::EMPTY)
    return;

  // Poll/read-back lane. read_queued_ remains true after the item leaves the
  // ring and while its request is in flight. Completion, error, mismatch, or
  // watchdog handling clears it.
  this->read_queue_.consume_front_if([this](VitoEntityBase *entity) {
    if (entity == nullptr) {
      ESP_LOGE(TAG, "null entity in read queue; dropping item");
      return true;
    }

    const optolink::Datapoint &datapoint = entity->get_datapoint();

#ifdef VITOHOME_PROTOCOL_GWG
    // GWG-only: entity->access() defaults to PHYSICAL (see
    // VitoEntityBase::access_), so an entity that never sets `access:` gets
    // exactly the pre-existing behaviour.
    const bool dispatched_ok = this->vito_->read(datapoint.address(), datapoint.length(), entity->access());
#elif defined(VITOHOME_HAS_RPC)
    // `access: rpc`: the same lane and the same response path as a read; only
    // the telegram differs. See VitoEntityBase::set_rpc_prefix().
    const bool dispatched_ok =
        entity->is_rpc() ? this->vito_->rpc(datapoint.address(), entity->rpc_prefix(), entity->rpc_prefix_len())
                         : this->vito_->read(datapoint.address(), datapoint.length());
#else
    const bool dispatched_ok = this->vito_->read(datapoint.address(), datapoint.length());
#endif
    if (!dispatched_ok) {
      // See the write lane above: transient _busy retains, a permanent
      // createPacket() rejection (unreachable for schema-validated poll
      // addresses) is dropped so the lane cannot spin. read_queued_ is cleared
      // so the poll scheduler can re-schedule it when next due.
      if (this->vito_->isBusy())
        return false;  // transient: retain at the head
      ESP_LOGE(TAG, "Engine refused read for %s (permanent); dropping", entity->get_datapoint().name());
      entity->read_queued_ = false;
      return true;  // permanent: drop
    }

    this->in_flight_ = entity;
    this->in_flight_op_ = OpType::READ;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();

    ESP_LOGV(TAG, "Dispatched read for %s", entity->get_datapoint().name());

    return true;
  });
}

bool VitoHomeComponent::request_priority_read(VitoEntityBase *entity) {
  if (entity == nullptr)
    return false;

  if (entity->read_queued_) {
    // Already pending in the read lane; a second copy would poll the same
    // datapoint twice. But "priority" must still mean "next": move the queued
    // copy to the head. Hardware, VScotHO1_72, 2026-10-15: a write ACKed during
    // a poll cycle whose polls already held the writer got its read-back only
    // after the whole cycle, 2.3 s later.
    // Not found means flagged but not in the lane (its read is in flight):
    // nothing to move, and a second copy would only repeat that read.
    static_cast<void>(this->read_queue_.move_to_front(entity));
    return true;
  }

  // Set the companion state before publishing the queue item, then roll it back
  // if the bounded lane rejects it -- otherwise the entity is wedged as
  // "queued" while absent from the queue. Same discipline as
  // schedule_due_entities_() and request_write().
  entity->read_queued_ = true;

  if (!this->read_queue_.push_front(entity)) {
    entity->read_queued_ = false;
    return false;
  }

  return true;
}

void VitoHomeComponent::schedule_due_entities_() {
  const uint32_t now = App.get_loop_component_start_time();

  // Two bugs lived in the old two-liner (`if (now < next_due) continue;
  // next_due = now + interval;`), both hardware-observed on VScotHO1_72 with
  // the SAME firmware binary across two 2026-07-09 logs:
  //
  //  1. `now` is sampled inside this callback, i.e. a few ms AFTER the
  //     ESPHome interval anchor that invoked update(). Re-anchoring the next
  //     due time on it made an entity whose update_interval EQUALS the hub
  //     tick (or an exact multiple of it) land a hair past the next tick, so
  //     whether it fired was decided by which tick carried more loop jitter --
  //     a coin flip. One log dropped the whole 60 s tier on tick 2; the other
  //     never dropped it. Anchoring on next_due_ms_ instead of `now` makes the
  //     schedule an arithmetic progression that cannot drift.
  //
  //  2. Even when it did fire, the period crept: each cycle added the
  //     accumulated jitter into the next due time.
  //
  // SLACK absorbs sub-tick jitter in the "is it due yet" test: anything due
  // within half a hub tick of now is treated as due now, because the next
  // opportunity to poll it is a full hub tick away and firing a few ms early
  // beats firing a whole tick late.
  const uint32_t slack = this->get_update_interval() / 2;

  std::size_t queued = 0;
  std::size_t skipped = 0;
  std::size_t rejected = 0;

  for (auto *entity : this->entities_) {
    if (entity == nullptr)
      continue;

    // VitoClock opts out: it is not polled on a datapoint interval but driven
    // by its own sync schedule, which pushes to the HEAD of the read lane.
    // Polling it here would both demote it behind every pending poll and run
    // the drift compare on the hub's tick instead of the sync interval.
    if (!entity->wants_polling())
      continue;

    if (entity->read_queued_) {
      ++skipped;
      continue;
    }

    const PollDecision decision = poll_schedule_step(now, entity->next_due_ms_, entity->poll_interval(), slack);

    if (!decision.due)
      continue;

    // Set the companion state before publishing the queue item. If the bounded
    // lane rejects it, roll the state back immediately so the entity is not
    // permanently wedged as "queued" while absent from the queue.
    entity->read_queued_ = true;

    if (!this->read_queue_.push_back(entity)) {
      entity->read_queued_ = false;
      ++rejected;
      continue;
    }

    // Advance the schedule only after insertion succeeded. Otherwise a
    // rejected push would suppress retry for a complete poll interval.
    if (entity->poll_interval() != 0)
      entity->next_due_ms_ = decision.next_due_ms;

    ++queued;
  }

  if (skipped != 0) {
    ESP_LOGW(TAG, "Poll cycle: %zu entities still queued from the previous cycle", skipped);
  }

  if (rejected != 0) {
    ESP_LOGE(TAG, "Poll cycle: read queue rejected %zu due entities (size=%zu, capacity=%zu)", rejected,
             this->read_queue_.size(), this->read_queue_.capacity());
  }

  ESP_LOGV(TAG, "Queued %zu reads", queued);
}

void VitoHomeComponent::update() {
  if (this->vito_ == nullptr || this->is_failed())
    return;

#ifdef VITOHOME_TIME_SYNC
  // Same tick the old time_sync_tick_() ran on, so the sync granularity is
  // unchanged: the hub's update_interval.
  this->clock_.tick(App.get_loop_component_start_time());
#endif

  if (this->entities_.empty())
    return;

  this->schedule_due_entities_();
}

void VitoHomeComponent::dump_config() {
  // Consecutive lines are emitted as ONE ESP_LOGCONFIG with \n separators --
  // the convention ESPHome core converged on (see wifi_component.cpp). Each
  // extra call site costs another TAG pointer and call sequence in flash, which
  // adds up across a component with eleven dump_config() bodies.
  //
  // The raw lane's slots are the component's single largest fixed allocation
  // (sizeof(RawOp) each), so the configured capacity is worth stating.
  ESP_LOGCONFIG(TAG,
                "VitoHome:\n"
                "  Protocol: %s\n"
                "  Entities: %zu\n"
                "  Raw queue capacity: %zu",
                PROTOCOL_NAME, this->entities_.size(), this->raw_queue_capacity_);

  if (this->ident_state_ == IdentState::DONE) {
    ESP_LOGCONFIG(TAG, "  Device: %s", this->ident_string_().c_str());
    char url[CATALOG_FINDER_URL_SIZE];
    if (this->ident_finder_url_(url, sizeof(url)))
      ESP_LOGCONFIG(TAG, "  Catalog finder: %s", url);
  }

#ifdef USE_TEXT_SENSOR
  if (this->raw_result_sensor_count_ > 0) {
    ESP_LOGCONFIG(TAG, "  Scan console: %u scan_result sensor(s) attached", this->raw_result_sensor_count_);
  }
#endif

  if (ESPHomeUARTInterface::frame_logging_enabled())
    ESP_LOGCONFIG(TAG, "  Frame logging: ON (tag 'vitohome.frames')");

  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup FAILED");

  // Deliberately NO loop over entities_ here: every entity is a registered
  // component, so ESPHome core already calls each one's dump_config() --
  // the old loop printed the entire entity list twice at boot
  // (hardware-observed, 2026-07-03 log).
  //
  // The clock is the one exception, and needs an explicit call: it is a
  // hub-owned VitoEntityBase, NOT a registered ESPHome component, so core's
  // fan-out never reaches it. The climate setpoint/mode channels are also
  // hub-owned VitoEntityBase entities in entities_ that core's fan-out misses,
  // but their dump_config() is empty; the clock is the only non-component
  // entity with anything to print, which is why this is one call and not a loop.
#ifdef VITOHOME_TIME_SYNC
  this->clock_.dump_config();
#endif
}

bool VitoHomeComponent::request_write(VitoEntityBase *entity) {
  // A procedure call carries its parameter, not a write payload, and may carry
  // none at all (VitoActionButton).
  if (entity == nullptr || (entity->write_length() == 0 && !entity->is_rpc()))
    return false;

  if (entity->write_queued_) {
    // Already in the write queue and not yet dispatched: control() has already
    // overwritten the entity buffer with the newest payload, so the pending
    // dispatch will transmit the latest value. Coalesce -- nothing to add.
    return true;
  }

  // Not queued. Either idle, or a write for this entity is currently in flight.
  // In the in-flight case the old bytes were already copied into the engine
  // packet, so the entity staging buffer now contains a newer value that must be
  // queued for a later transaction.
  //
  // Set the logical state before publishing the queue entry, then roll it back
  // if the bounded insertion is rejected.
  entity->write_queued_ = true;

  if (!this->write_queue_.push_back(entity)) {
    entity->write_queued_ = false;

    ESP_LOGE(TAG, "write queue full (%zu/%zu); write for %s was not queued", this->write_queue_.size(),
             this->write_queue_.capacity(), entity->get_datapoint().name());

    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Raw scan console (debug)
// ---------------------------------------------------------------------------

void VitoHomeComponent::queue_raw_read(uint16_t address, uint8_t length, optolink::GWGAccessMode access) {
  if (length < 1 || length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_read: length %u out of range (1..%u)", length, RAW_READ_MAX);
    return;
  }

  this->enqueue_raw_(address, length, false, nullptr, 0, access);
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

  this->enqueue_raw_(address, static_cast<uint8_t>(len), true, data, static_cast<uint8_t>(len), access);
}

void VitoHomeComponent::queue_raw_write(uint16_t address, const std::vector<uint8_t> &bytes,
                                        optolink::GWGAccessMode access) {
  // Ergonomic overload for callers that already hold a vector (e.g. a lambda's
  // braced-init-list). Forwards to the pointer/length form so both share one
  // validation-and-enqueue path.
  this->queue_raw_write(address, bytes.data(), bytes.size(), access);
}

void VitoHomeComponent::queue_raw_rpc(uint16_t address, const uint8_t *parameter, std::size_t len) {
#ifdef VITOHOME_HAS_RPC
  // Same cap as a raw write: the parameter rides in RawOp::bytes. Zero is
  // allowed -- a procedure without a parameter -- and then runs the procedure.
  if (len > RAW_WRITE_MAX || (len != 0 && parameter == nullptr)) {
    ESP_LOGW(TAG, "queue_raw_rpc: %zu parameter bytes out of range (0..%u)", len, RAW_WRITE_MAX);
    return;
  }
  this->enqueue_raw_(address, static_cast<uint8_t>(len), false, parameter, static_cast<uint8_t>(len),
                     optolink::GWGAccessMode::PHYSICAL, true);
#else
  (void) parameter;
  (void) len;
  ESP_LOGW(TAG, "queue_raw_rpc: 0x%04X not sent -- %s has no remote procedure call telegram", address, PROTOCOL_NAME);
#endif
}

void VitoHomeComponent::queue_raw_rpc(uint16_t address, const std::vector<uint8_t> &parameter) {
  this->queue_raw_rpc(address, parameter.data(), parameter.size());
}

bool VitoHomeComponent::enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t *bytes,
                                     uint8_t bytes_len, optolink::GWGAccessMode access, bool is_rpc) {
  // An RPC's length is its parameter's, which may be empty; see queue_raw_rpc().
  if (length == 0 && !is_rpc) {
    ESP_LOGW(TAG, "enqueue_raw_: zero-length operation rejected");
    return false;
  }

  if (bytes_len > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "enqueue_raw_: %u bytes exceeds %u; dropping", bytes_len, RAW_WRITE_MAX);
    return false;
  }

  if (is_rpc) {
    if (is_write || bytes_len != length || (bytes_len != 0 && bytes == nullptr)) {
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
  operation.access = access;  // read and write TYPE byte on GWG; ignored on other builds
  operation.is_rpc = is_rpc;

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
               raw_kind(is_write, is_rpc), address);
    } else {
      ESP_LOGW(TAG, "raw queue full (%zu/%zu); dropping %s 0x%04X. Raise raw_queue_size for larger sweeps.",
               this->raw_queue_.size(), this->raw_queue_.capacity(), raw_kind(is_write, is_rpc), address);
    }

    return false;
  }

  ESP_LOGD(TAG, "Queued raw %s 0x%04X len %u", raw_kind(is_write, is_rpc), address, length);
  return true;
}

void VitoHomeComponent::raw_handle_response_(const ResponseView &response) {
  // "0xXXXX:" (7) + RAW_READ_MAX * 3 hex chars + the integer views for widths
  // 1..8 + NUL. 208 covers a 48-byte dump with room to spare.
  char buffer[208];

  if (this->raw_is_write_) {
    snprintf(buffer, sizeof(buffer), "0x%04X: write ACK (%u byte%s)", this->raw_dp_.address(), this->raw_write_len_,
             this->raw_write_len_ == 1 ? "" : "s");
  } else {
    format_raw_dump(response.address, response.data, response.data_length, buffer, sizeof(buffer));
  }

  ESP_LOGI(TAG, "Raw: %s", buffer);
  this->raw_publish_(buffer);
}

void VitoHomeComponent::raw_handle_error_(optolink::OptolinkResult error) {
  char buffer[96];

  snprintf(buffer, sizeof(buffer), "0x%04X: %s FAILED (%s)", this->raw_dp_.address(),
           raw_kind(this->raw_is_write_, this->raw_is_rpc_), optolink::errorToString(error));

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

// ---------------------------------------------------------------------------
// Engine callbacks: response / error routing
// ---------------------------------------------------------------------------
// Every completed engine transaction lands here and is routed to its owner:
// the identification state machine, the raw scan-console lane, or the
// in-flight entity (which since the NTP refactor includes VitoClock -- system
// time sync is an ordinary entity on the read/write lanes, not a raw-lane
// rider).

void VitoHomeComponent::on_response_(const ResponseView &response, uint16_t request_address) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;
    this->ident_handle_response_(response);
    return;
  }

  if (this->raw_in_flight_) {
    this->raw_in_flight_ = false;

    // P300 carries a real response address. KW/GWG echo the request address, so
    // the same check is harmless there. Reject a mismatched raw response rather
    // than publishing or feeding it into the clock state machine.
    if (response.address != this->raw_dp_.address()) {
      ESP_LOGW(TAG, "Raw response address 0x%04X does not match in-flight 0x%04X; dropping", response.address,
               this->raw_dp_.address());
      this->raw_handle_error_(optolink::OptolinkResult::ERROR);
      return;
    }

    this->raw_handle_response_(response);
    return;
  }

  VitoEntityBase *entity = this->in_flight_;
  const OpType operation = this->in_flight_op_;

  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  if (entity == nullptr) {
    ESP_LOGW(TAG, "Response received for 0x%04X but no in-flight request", request_address);
    return;
  }

  // A write was dispatched to the command address; a read to the state
  // address. Match the response against whichever this operation used.
  //
  // On P300 response.address is the address echoed in the device's own frame,
  // so this is a live wire-level check. KW/GWG carry no response address and
  // echo the retained request address, making the check tautological there.
  // An RMW_READ is a read of the WRITE register (see the write lane).
  const uint16_t expected_address = (operation == OpType::WRITE || operation == OpType::RMW_READ)
                                        ? entity->get_write_datapoint().address()
                                        : entity->get_datapoint().address();

  if (expected_address != response.address) {
    ESP_LOGW(TAG, "Response address 0x%04X does not match in-flight 0x%04X; dropping", response.address,
             expected_address);

    // Clear only the state belonging to this operation (if a newer write was
    // queued while an older one was in flight, write_queued_ remains set) and
    // notify the entity exactly as on_error_ does for the same operation. The
    // notification is not optional: a drop without it left VitoClock wedged
    // permanently -- its tick() refuses to start while phase_ != IDLE and,
    // unlike polled entities, nothing ever re-queues it -- so one crossed
    // response during READING/VERIFYING silently killed time sync until
    // reboot. handle_error() is VitoClock's designed recovery hook (it resets
    // the phase), and for streak-tracking entities (sensor/number) a dropped
    // read is a failed read, same as a timeout. ERROR matches the raw lane's
    // mismatch path above; the specific code is advisory (no handler branches
    // on it). Link health is unaffected either way: link_note_alive_() already
    // ran for this frame in the onResponse callback, which is correct -- a
    // checksum-valid, mis-addressed frame still proves the peer answers.
    if (operation == OpType::READ) {
      if (this->rmw_verify_entity_ == entity)
        this->rmw_verify_entity_ = nullptr;
      entity->read_queued_ = false;
      entity->handle_error(optolink::OptolinkResult::ERROR);
    } else if (operation == OpType::WRITE) {
      if (this->rmw_verify_entity_ == entity)
        this->rmw_verify_entity_ = nullptr;
      entity->write_in_flight_ = false;
      entity->handle_write_error(optolink::OptolinkResult::ERROR);
    } else if (operation == OpType::RMW_READ) {
      // Fail closed: a byte read from the wrong address must never be merged
      // and written back.
      this->rmw_abort_(entity, optolink::OptolinkResult::ERROR, "read-before-write answered for another address");
    }

    return;
  }

  if (operation == OpType::RMW_READ) {
    this->rmw_handle_read_(entity, response);
    return;
  }

  if (operation == OpType::WRITE) {
    entity->write_in_flight_ = false;

    ESP_LOGD(TAG, "Write to %s acknowledged", entity->get_datapoint().name());

    // If control() queued a newer value while this request was in flight, this
    // ACK belongs to the older payload. The entity's pending_* member and write
    // buffer already describe the NEWER value.
    //
    // Calling handle_write_response() here could therefore publish a value the
    // device has not accepted yet when read_back is disabled. Likewise, an
    // immediate read-back would observe the intermediate device state before
    // the newer queued write. Let the newer write retain priority; its own ACK
    // will perform publication or read-back.
    if (entity->write_queued_) {
      ESP_LOGV(TAG, "Write ACK for %s superseded by a newer queued value", entity->get_datapoint().name());
      if (this->rmw_verify_entity_ == entity)
        this->rmw_verify_entity_ = nullptr;  // the newer write re-reads first
      return;
    }

    entity->handle_write_response(response);
    if (this->rmw_verify_entity_ == entity && !entity->wants_read_back())
      this->rmw_verify_entity_ = nullptr;  // no read-back is coming

    // Siblings first, then the writer's own read-back pushed (or, if its poll
    // is already queued, moved) in front of them: the writer's read-back is the
    // one the check above is waiting for.
    this->refresh_siblings_(entity);

    if (entity->wants_read_back()) {
      // Confirm by reading the device's view of the value, ahead of the regular
      // poll queue.
      if (!this->request_priority_read(entity)) {
        ESP_LOGE(TAG, "read queue full (%zu/%zu); immediate read-back for %s was not queued", this->read_queue_.size(),
                 this->read_queue_.capacity(), entity->get_datapoint().name());
      }
    }

    return;
  }

  if (operation == OpType::READ) {
    entity->read_queued_ = false;
    if (this->rmw_verify_entity_ == entity)
      this->rmw_verify_read_back_(entity, response);
    entity->handle_response(response);
    return;
  }

  ESP_LOGW(TAG, "Response received for %s with no valid operation type", entity->get_datapoint().name());
}

// ---------------------------------------------------------------------------
// Read-modify-write, bit fields and whole blocks (see rmw_pending_ in vitohome.h)
// ---------------------------------------------------------------------------

void VitoHomeComponent::rmw_abort_(VitoEntityBase *entity, optolink::OptolinkResult error, const char *why) {
  ESP_LOGW(TAG, "%s: read-modify-write NOT sent: %s", entity->get_datapoint().name(), why);
  entity->write_in_flight_ = false;
  entity->handle_write_error(error);
}

void VitoHomeComponent::rmw_handle_read_(VitoEntityBase *entity, const ResponseView &response) {
  // Guard on data() before reading the payload (docs/design_notes.md SS10a,
  // finding 3: a null data() can carry a non-zero length).
  const uint8_t block_len = entity->get_write_datapoint().length();
  const bool block_mode = entity->block_field_width() != 0;
  const uint8_t offset = block_mode ? entity->block_field_offset() : 0;
  const uint8_t width = block_mode ? entity->block_field_width() : 1;
  if (response.data == nullptr || response.data_length < block_len || block_len > MAX_RMW_BLOCK ||
      entity->write_length() < width) {
    this->rmw_abort_(entity, optolink::OptolinkResult::LENGTH, "short read-before-write response");
    return;
  }
#ifdef VITOHOME_PROTOCOL_GWG
  // An EEPROM FF FF pair means "address not in use" and is delivered as 0xFF,
  // which is indistinguishable from a byte with every flag set. Merging into it
  // and writing it back would turn an unserved address into a served one, so
  // the engine flags the pair (GWGEngine::lastReadHadUnservedEeprom) and the
  // write stops here. Ordinary reads are unaffected.
  if (this->vito_->lastReadHadUnservedEeprom()) {
    this->rmw_abort_(entity, optolink::OptolinkResult::ERROR,
                     "GWG EEPROM answered FF FF (address not in use) - refusing to merge into it");
    return;
  }
#endif
  if (!merge_block_field(response.data, block_len, offset, width, entity->field_mask(), entity->write_data(),
                         this->rmw_buf_)) {
    // Unreachable through validated config (geometry and value are checked in
    // Python and again by the entity); kept fail-closed.
    this->rmw_abort_(entity, optolink::OptolinkResult::ERROR, "field does not fit its block or bit_mask");
    return;
  }
  this->rmw_len_ = block_len;
  this->rmw_pending_ = entity;  // dispatch_next_() sends it before anything else
  char old_hex[format_hex_pretty_size(MAX_RMW_BLOCK)];
  char new_hex[format_hex_pretty_size(MAX_RMW_BLOCK)];
  format_hex_pretty_to(old_hex, response.data, block_len, ' ');
  format_hex_pretty_to(new_hex, this->rmw_buf_, block_len, ' ');
  ESP_LOGD(TAG, "%s: read-modify-write %s -> %s (field %u byte(s) at %u, bit_mask 0x%02X)",
           entity->get_datapoint().name(), old_hex, new_hex, width, offset, entity->field_mask());
}

void VitoHomeComponent::rmw_dispatch_write_() {
  VitoEntityBase *entity = this->rmw_pending_;
  const optolink::Datapoint &wdp = entity->get_write_datapoint();
#ifdef VITOHOME_PROTOCOL_GWG
  const bool write_ok = this->vito_->write(wdp.address(), this->rmw_buf_, this->rmw_len_, entity->access());
#else
  const bool write_ok = this->vito_->write(wdp.address(), this->rmw_buf_, this->rmw_len_);
#endif
  if (!write_ok) {
    // Transient: stay armed. dispatch_next_() keeps returning here, so nothing
    // else reaches the bus until the merged write is out.
    if (this->vito_->isBusy())
      return;
    this->rmw_pending_ = nullptr;
    this->rmw_abort_(entity, optolink::OptolinkResult::ERROR, "engine refused the merged write (permanent)");
    return;
  }
  this->rmw_pending_ = nullptr;
  this->in_flight_ = entity;
  this->in_flight_op_ = OpType::WRITE;
  this->in_flight_started_ms_ = App.get_loop_component_start_time();
  // From here the ordinary WRITE completion paths apply (ACK, read-back,
  // supersede, error, watchdog). Arm the read-back check.
  this->rmw_verify_entity_ = entity;
  std::memcpy(this->rmw_verify_buf_, this->rmw_buf_, this->rmw_len_);
  this->rmw_verify_len_ = this->rmw_len_;
  ESP_LOGV(TAG, "Dispatched merged write for %s (%u bytes)", entity->get_datapoint().name(), this->rmw_len_);
}

void VitoHomeComponent::rmw_verify_read_back_(VitoEntityBase *entity, const ResponseView &response) {
  this->rmw_verify_entity_ = nullptr;
  const uint8_t len = this->rmw_verify_len_;
  if (response.data == nullptr || response.data_length < len)
    return;  // the entity's own decode path reports a short read
  // Per byte: which bits belong to the field this entity wrote. Everything else
  // must read back exactly as written.
  const bool block_mode = entity->block_field_width() != 0;
  const uint8_t offset = block_mode ? entity->block_field_offset() : 0;
  const uint8_t width = block_mode ? entity->block_field_width() : 1;
  const uint8_t mask = entity->field_mask();
  bool outside_changed = false;
  bool field_changed = false;
  for (uint8_t i = 0; i < len; i++) {
    const bool in_field = i >= offset && i < offset + width;
    const uint8_t field_bits = in_field ? (mask != 0 ? mask : 0xFF) : 0x00;
    const uint8_t diff = static_cast<uint8_t>(response.data[i] ^ this->rmw_verify_buf_[i]);
    if (diff & static_cast<uint8_t>(~field_bits))
      outside_changed = true;
    if (diff & field_bits)
      field_changed = true;
  }
  char got_hex[format_hex_pretty_size(MAX_RMW_BLOCK)];
  char put_hex[format_hex_pretty_size(MAX_RMW_BLOCK)];
  format_hex_pretty_to(got_hex, response.data, len, ' ');
  format_hex_pretty_to(put_hex, this->rmw_verify_buf_, len, ' ');
  if (outside_changed) {
    ESP_LOGW(TAG,
             "%s: read-back %s differs from the written %s OUTSIDE the field -- another datapoint in this block "
             "changed between the read and the write, or the device did not take the write as sent",
             entity->get_datapoint().name(), got_hex, put_hex);
  } else if (field_changed) {
    ESP_LOGW(TAG, "%s: device reports %s after writing %s -- the field did not take", entity->get_datapoint().name(),
             got_hex, put_hex);
  } else {
    ESP_LOGD(TAG, "%s: read-back %s confirms the merged write", entity->get_datapoint().name(), got_hex);
  }
}

void VitoHomeComponent::refresh_siblings_(VitoEntityBase *writer) {
  const uint16_t address = writer->get_write_datapoint().address();
  unsigned queued = 0;
  for (auto *e : this->entities_) {
    // Not the writer (its own read-back is queued separately), and not an
    // entity that opts out of polling: VitoClock drives its own state machine
    // and must not receive an unsolicited response.
    if (e == nullptr || e == writer || !e->wants_polling() || e->get_datapoint().address() != address)
      continue;
#ifdef VITOHOME_PROTOCOL_GWG
    // GWG addresses are per access mode: 0x01/eeprom and 0x01/port are
    // different registers.
    if (e->access() != writer->access())
      continue;
#endif
    // An RPC answer is computed by the controller, not read from the memory a
    // write changed, so sharing the address says nothing about it.
    if (e->is_rpc())
      continue;
    if (this->request_priority_read(e))
      ++queued;
  }
  if (queued > 0)
    ESP_LOGD(TAG, "%s: refreshing %u entit%s sharing 0x%04X", writer->get_datapoint().name(), queued,
             queued == 1 ? "y" : "ies", address);
}

bool VitoHomeComponent::refresh_all() {
  // Fresh millis() is deliberate here (not
  // App.get_loop_component_start_time()). refresh_all() is a rare cold path
  // entered from a button press or user lambda, potentially from another
  // component's loop dispatch rather than the hub's own callback.
  const uint32_t now = millis();

  if (this->last_refresh_all_ms_ != 0 && now - this->last_refresh_all_ms_ < REFRESH_ALL_MIN_INTERVAL_MS) {
    ESP_LOGW(TAG, "refresh_all() suppressed (last one %" PRIu32 " ms ago, min interval %" PRIu32 " ms)",
             now - this->last_refresh_all_ms_, REFRESH_ALL_MIN_INTERVAL_MS);
    return false;
  }

  this->last_refresh_all_ms_ = now;

  // Only the poll rotation is refreshed. next_due_ms_ is the poll scheduler's
  // field, and an entity that opts out of polling does not read it -- VitoClock
  // is the case in point: it drives itself from next_sync_ms_ on the hub tick
  // and says so at vito_clock.h ("Deliberately not next_due_ms_"). Writing the
  // field on it was harmless but untrue to the intent, and would have quietly
  // become load-bearing the day anything non-polling started reading it. Guard
  // on the same predicate the scheduler itself uses so the two cannot drift.
  unsigned refreshed = 0;
  for (auto *entity : this->entities_) {
    if (entity == nullptr || !entity->wants_polling())
      continue;
    entity->next_due_ms_ = 0;
    ++refreshed;
  }

  ESP_LOGI(TAG, "refresh_all(): %u entities marked due; queue drains at normal pace", refreshed);

  return true;
}

void VitoHomeComponent::publish_link_(bool up) {
  const int8_t next = up ? 1 : 0;

  if (this->link_state_ == next)
    return;

  this->link_state_ = next;

  ESP_LOGI(TAG, "Optolink link %s", up ? "online" : "offline");

#ifdef USE_BINARY_SENSOR
  for (uint16_t i = 0; i < this->link_sensor_count_; i++) {
    if (this->link_sensors_[i] != nullptr)
      this->link_sensors_[i]->publish_state(up);
  }
#endif
}

void VitoHomeComponent::link_note_alive_() {
  this->link_established_ = true;
  this->link_error_streak_ = 0;
  this->publish_link_(true);
  // Mirror the link state onto the STANDARD component status as well, not only
  // onto the optional connectivity binary_sensor. The binary_sensor is opt-in,
  // so without this a user who did not configure one gets no indication at all
  // that the hub is up but the peer has gone silent -- no status LED, no
  // component-status diagnostic in Home Assistant.
  this->status_clear_warning();
}

#ifdef VITOHOME_PROTOCOL_GWG
void VitoHomeComponent::gwg_note_all_ff_(const uint8_t *data, uint8_t length) {
  // "Link established" is true and useless when every datapoint reads 0xFF.
  //
  // GWG has no error telegram: a request for an address the unit does not
  // serve in the requested access mode is answered with 0xFF rather than
  // refused, so the engine completes the transaction, the hub marks the link
  // alive, and the operator is told the protocol works. Hardware capture
  // 2026-10-10 (GWG_VBEM SW=0x7A) read 13 BE-mode addresses that the device's
  // own Vitosoft catalog lists by name and got 0xFF from every one, under a
  // cheerful "GWG link established".
  //
  // uwen70/esphome_vitoconnect reaches the same reading from the other
  // direction: its EEPROM path treats a returned FF FF pair as "address not in
  // use" rather than as data.
  //
  // Deliberately conservative. The streak counts only whole responses that are
  // 0xFF in every byte, resets on the first response that is not, and warns
  // once per session at the threshold. A datapoint that legitimately reads
  // 0xFF (an unset enum, a disconnected sensor) will not trip it unless the
  // configured catalog is ENTIRELY such datapoints, in which case the warning
  // is earned.
  if (length == 0)
    return;

  for (uint8_t i = 0; i < length; i++) {
    if (data[i] != 0xFF) {
      this->gwg_all_ff_streak_ = 0;
      return;
    }
  }

  if (this->gwg_all_ff_warned_)
    return;

  if (this->gwg_all_ff_streak_ < GWG_ALL_FF_WARN_AFTER)
    ++this->gwg_all_ff_streak_;

  if (this->gwg_all_ff_streak_ == GWG_ALL_FF_WARN_AFTER) {
    this->gwg_all_ff_warned_ = true;
    ESP_LOGW(TAG,
             "GWG: the last %u responses were 0xFF in every byte. The link is up, but the unit is "
             "most likely not serving these addresses in the requested access mode -- GWG answers "
             "an unserved address with 0xFF rather than an error. Check the `access:` mode before "
             "trusting any of these values, and do not write to them.",
             static_cast<unsigned>(GWG_ALL_FF_WARN_AFTER));
  }
}
#endif

void VitoHomeComponent::link_note_error_() {
  if (this->link_error_streak_ < LINK_OFFLINE_AFTER_ERRORS)
    ++this->link_error_streak_;

  if (this->link_error_streak_ == LINK_OFFLINE_AFTER_ERRORS) {
    this->publish_link_(false);
    // Same edge as publish_link_(false): raised once when the streak crosses
    // the threshold, cleared by link_note_alive_(). See the note there.
    this->status_set_warning("optolink link offline");
  }
}

void VitoHomeComponent::on_error_(optolink::OptolinkResult error, uint16_t request_address) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;

    ESP_LOGD(TAG, "Identification read 0x%04X failed (%s)", request_address, optolink::errorToString(error));

    this->ident_handle_error_();
    return;
  }

  if (this->raw_in_flight_) {
    this->raw_in_flight_ = false;
    this->raw_handle_error_(error);
    return;
  }

  VitoEntityBase *entity = this->in_flight_;
  const OpType operation = this->in_flight_op_;

  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  // Name for the log line: the in-flight entity's datapoint when there is one,
  // else the echoed request address for a stray callback.
  const char *name = entity != nullptr ? entity->get_datapoint().name() : "?";

  switch (error) {
    case optolink::OptolinkResult::TIMEOUT:
      ESP_LOGE(TAG, "[TIMEOUT] %s (0x%04X) - Optolink not responding", name, request_address);
      break;

    case optolink::OptolinkResult::LENGTH:
      ESP_LOGE(TAG, "[LENGTH]  %s (0x%04X) - invalid payload length", name, request_address);
      break;

    case optolink::OptolinkResult::NACK:
      ESP_LOGW(TAG,
               "[NACK]    %s (0x%04X) — heater rejected request "
               "(unsupported address?)",
               name, request_address);
      break;

    case optolink::OptolinkResult::CRC:
      ESP_LOGE(TAG, "[CRC]     %s (0x%04X) - checksum mismatch", name, request_address);
      break;

    case optolink::OptolinkResult::DEVICE_ERROR:
      ESP_LOGW(TAG, "[DEVERR]  %s (0x%04X) - device returned an error frame", name, request_address);
      break;

    case optolink::OptolinkResult::ERROR:
    default:
      ESP_LOGE(TAG, "[ERROR]   %s (0x%04X) - protocol error", name, request_address);
      break;
  }

  if (entity == nullptr)
    return;

  if (operation == OpType::READ) {
    if (this->rmw_verify_entity_ == entity)
      this->rmw_verify_entity_ = nullptr;  // the read-back failed; never verify a later poll against it
    entity->read_queued_ = false;
    entity->handle_error(error);
  } else if (operation == OpType::WRITE) {
    if (this->rmw_verify_entity_ == entity)
      this->rmw_verify_entity_ = nullptr;
    entity->write_in_flight_ = false;

    // The device value did not change on a failed write. The entity keeps its
    // published state by default rather than going unavailable. A newer write
    // that was queued during this transaction remains queued.
    entity->handle_write_error(error);
  } else if (operation == OpType::RMW_READ) {
    // Fail closed: without the current byte there is nothing safe to merge
    // into, so the write is abandoned rather than sent blind.
    this->rmw_abort_(entity, error, "read-before-write failed");
  } else {
    ESP_LOGW(TAG, "Error callback for %s had no valid operation type", name);
  }
}

// ---------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------

void VitoHomeComponent::ident_start_() {
  this->ident_state_ = IdentState::READ4;
  this->ident_dispatch_(IdentState::READ4);
}

void VitoHomeComponent::ident_dispatch_(IdentState state) {
  this->ident_state_ = state;

  switch (state) {
    case IdentState::READ4:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F8, 4, optolink::noconv);
      break;

    case IdentState::READ_F8:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F8, 1, optolink::noconv);
      break;

    case IdentState::READ_F9:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F9, 1, optolink::noconv);
      break;

    case IdentState::READ_FA:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00FA, 1, optolink::noconv);
      break;

    case IdentState::READ_FB:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00FB, 1, optolink::noconv);
      break;

    case IdentState::READ_GFA:
      this->ident_dp_ = optolink::Datapoint("ident", 0x7650, 1, optolink::noconv);
      break;

    case IdentState::READ_F0:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F0, 2, optolink::noconv);
      break;

    default:
      break;
  }

  // The actual bus dispatch happens from dispatch_next_() when idle.
}

void VitoHomeComponent::ident_handle_response_(const ResponseView &response) {
  // P300 carries the actual response address. KW/GWG echo the request address,
  // so this check is harmless for those protocols.
  if (response.address != this->ident_dp_.address()) {
    ESP_LOGW(TAG, "Identification response address 0x%04X does not match in-flight 0x%04X", response.address,
             this->ident_dp_.address());
    this->ident_handle_error_();
    return;
  }

  const uint8_t *data = response.data;
  const uint8_t length = response.data_length;

  // A non-zero payload length must have usable storage. Treat a malformed view
  // as a failed identification step rather than dereferencing nullptr.
  if (length != 0 && data == nullptr) {
    ESP_LOGW(TAG, "Identification response has length %u but no payload", length);
    this->ident_handle_error_();
    return;
  }

  switch (this->ident_state_) {
    case IdentState::READ4:
      // 0xF8..0xFB in one transaction. Wire order is register order:
      // F8 = group, F9 = controller, FA = HW index, FB = SW index.
      if (length >= 4) {
        this->ident_group_ = data[0];
        this->ident_controller_ = data[1];
        this->ident_hw_ = data[2];
        this->ident_sw_ = data[3];
        this->ident_after_sw_();
        return;
      }

      // Short response: fall back to single-byte reads.
      this->ident_dispatch_(IdentState::READ_F8);
      return;

    case IdentState::READ_F8:
      if (length >= 1)
        this->ident_group_ = data[0];

      this->ident_dispatch_(IdentState::READ_F9);
      return;

    case IdentState::READ_F9:
      if (length >= 1)
        this->ident_controller_ = data[0];

      this->ident_dispatch_(IdentState::READ_FA);
      return;

    case IdentState::READ_FA:
      if (length >= 1)
        this->ident_hw_ = data[0];

      this->ident_dispatch_(IdentState::READ_FB);
      return;

    case IdentState::READ_FB:
      if (length >= 1)
        this->ident_sw_ = data[0];

      this->ident_after_sw_();
      return;

    case IdentState::READ_GFA:
      // 0x00 and 0xFF are not chips: KW answers an address the unit does not
      // have with 0xFF fill, and 0x00 is an unprogrammed cell. P300 answers
      // with an error frame instead (ident_handle_error_).
      if (length >= 1 && data[0] != 0x00 && data[0] != 0xFF)
        this->ident_gfa_ = data[0];
      else
        ESP_LOGD(TAG, "No burner-control chip at 0x7650");
      this->ident_dispatch_(IdentState::READ_F0);
      return;

    case IdentState::READ_F0:
      // Raw and uninterpreted: reading F0 from the two bytes (Vitosoft: one
      // little-endian Int) and what it selects is the catalog finder's
      // business. FF FF is KW's
      // answer for an address the unit does not have; 00 00 is a real value
      // (the generic variant of a family F0 subdivides).
      if (length >= 2 && !(data[0] == 0xFF && data[1] == 0xFF))
        this->ident_f0_ = (data[0] << 8) | data[1];
      else
        ESP_LOGD(TAG, "No extended identification (F0) at 0x00F0");
      this->ident_finish_();
      return;

    default:
      return;
  }
}

void VitoHomeComponent::ident_handle_error_() {
  // Fail-soft per step: the multi-byte read degrades to single-byte reads
  // (length-1 reads at F8/F9 are wire-confirmed on the reference unit), and
  // each single-byte failure just leaves that field unknown.
  switch (this->ident_state_) {
    case IdentState::READ4:
      this->ident_dispatch_(IdentState::READ_F8);
      return;

    case IdentState::READ_F8:
      this->ident_dispatch_(IdentState::READ_F9);
      return;

    case IdentState::READ_F9:
      this->ident_dispatch_(IdentState::READ_FA);
      return;

    case IdentState::READ_FA:
      this->ident_dispatch_(IdentState::READ_FB);
      return;

    case IdentState::READ_FB:
      this->ident_after_sw_();
      return;

    case IdentState::READ_GFA:
      // A unit without a burner control (heat pump, ...) has no 0x7650.
      this->ident_dispatch_(IdentState::READ_F0);
      return;

    case IdentState::READ_F0:
      // Most units have no 0x00F0; only a few variant families declare it.
      this->ident_finish_();
      return;

    default:
      return;
  }
}

void VitoHomeComponent::ident_after_sw_() {
#ifdef VITOHOME_PROTOCOL_GWG
  // GWG addresses a single byte: 0x7650 does not exist there.
  this->ident_finish_();
#else
  this->ident_dispatch_(IdentState::READ_GFA);
#endif
}

std::string VitoHomeComponent::ident_string_() const {
  char buffer[96];

  if (this->ident_group_ >= 0 && this->ident_controller_ >= 0) {
    const uint16_t ident = static_cast<uint16_t>((this->ident_group_ << 8) | this->ident_controller_);

    const char *family = ident_family_name(ident, this->ident_hw_);

    int offset = snprintf(buffer, sizeof(buffer), "0x%04X%s%s%s", ident, family != nullptr ? " (" : "",
                          family != nullptr ? family : "", family != nullptr ? ")" : "");

    // HW and SW are printed independently, matching ident_finder_url_(): a
    // unit that answers 0xFA but not 0xFB still shows its hardware index.
    if (this->ident_hw_ >= 0 && offset > 0 && offset < static_cast<int>(sizeof(buffer)))
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, " HW=0x%02X", this->ident_hw_);
    if (this->ident_sw_ >= 0 && offset > 0 && offset < static_cast<int>(sizeof(buffer)))
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, " SW=0x%02X", this->ident_sw_);
    if (this->ident_gfa_ >= 0 && offset > 0 && offset < static_cast<int>(sizeof(buffer))) {
      const char *chip = gfa_chip_name(this->ident_gfa_);
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, " GFA=0x%02X%s%s%s", this->ident_gfa_,
                         chip != nullptr ? " (" : "", chip != nullptr ? chip : "", chip != nullptr ? ")" : "");
    }
    if (this->ident_f0_ >= 0 && offset > 0 && offset < static_cast<int>(sizeof(buffer)))
      snprintf(buffer + offset, sizeof(buffer) - offset, " F0=0x%04X", this->ident_f0_);

    return std::string(buffer);
  }

  return std::string("unknown (identification reads failed)");
}

bool VitoHomeComponent::ident_finder_url_(char *buffer, size_t size) const {
  if (this->ident_group_ < 0 || this->ident_controller_ < 0)
    return false;

  int offset =
      snprintf(buffer, size, "%s?id=%02X%02X", CATALOG_FINDER_URL, this->ident_group_, this->ident_controller_);
  if (this->ident_hw_ >= 0 && offset > 0 && offset < static_cast<int>(size))
    offset += snprintf(buffer + offset, size - offset, "&hw=%02X", this->ident_hw_);
  if (this->ident_sw_ >= 0 && offset > 0 && offset < static_cast<int>(size))
    offset += snprintf(buffer + offset, size - offset, "&sw=%02X", this->ident_sw_);
  if (this->ident_gfa_ >= 0 && offset > 0 && offset < static_cast<int>(size))
    offset += snprintf(buffer + offset, size - offset, "&gfa=%02X", this->ident_gfa_);
  if (this->ident_f0_ >= 0 && offset > 0 && offset < static_cast<int>(size))
    snprintf(buffer + offset, size - offset, "&f0=%04X", this->ident_f0_);

  return true;
}

void VitoHomeComponent::ident_finish_() {
  this->ident_state_ = IdentState::DONE;

  const std::string identification = this->ident_string_();

  ESP_LOGI(TAG, "Device identification: %s", identification.c_str());

  char url[CATALOG_FINDER_URL_SIZE];
  if (this->ident_finder_url_(url, sizeof(url)))
    ESP_LOGI(TAG, "Find the matching datapoint catalog: %s", url);

  if (this->ident_sw_ < 0 && this->ident_group_ >= 0) {
    ESP_LOGI(TAG, "Software index (0xFB) unavailable - when picking datapoints from the "
                  "Vitosoft data, match on the family only and verify on the wire.");
  }

#ifdef USE_TEXT_SENSOR
  for (uint16_t i = 0; i < this->device_id_sensor_count_; i++) {
    if (this->device_id_sensors_[i] != nullptr)
      this->device_id_sensors_[i]->publish_state(identification);
  }
#endif
}

}  // namespace esphome::vitohome
