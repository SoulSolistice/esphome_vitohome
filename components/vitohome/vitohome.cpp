#include "vitohome.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

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
  // 8E2 near 4800 baud is enforced at config time (FINAL_VALIDATE_SCHEMA in __init__.py),
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
  // Not std::make_unique: without exceptions an allocation failure there
  // aborts the firmware, where the queues above fail soft.
  this->vito_.reset(new (std::nothrow) optolink::OptolinkEngine<SelectedProtocol>(&this->iface_));
  if (!this->vito_) {
    ESP_LOGE(TAG, "failed to allocate the protocol engine");
    this->mark_failed(LOG_STR("engine allocation failed"));
    return;
  }
  this->apply_gwg_diagnostics_();

  // All three engines share one byte-mover callback shape, so the hub registers
  // directly with the engine and wraps the raw payload in a ResponseView here.
  //
  // On P300 `address` is the one echoed in the device's own response frame (a
  // real wire-level datum); on KW/GWG the engine echoes the retained request
  // address. The engine is strictly single-in-flight and the hub tracks its own
  // in-flight context (in_flight_op_ and, for an entity, in_flight_), so the
  // callback carries only that address as a wire-level cross-check.
  this->vito_->onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
    // A complete valid response proves link liveness and validates the
    // configured protocol for start-up verification.
    this->link_note_alive_();
#ifdef VITOHOME_PROTOCOL_GWG
    this->gwg_note_all_ff_(data, length);
#endif
    ++this->answers_total_;
    this->on_response_(ResponseView{data, length, address}, address);
  });

  this->vito_->onError([this](optolink::OptolinkResult error, uint16_t request_address) {
    // Link health tracks a persistent no-response condition rather than every
    // failed operation:
    //
    //   * NACK means the device received the request and actively rejected it.
    //   * DEVICE_ERROR is a COMPLETE, checksum-valid error frame from the
    //     device -- proof the peer answered in this protocol.
    //   * TIMEOUT means no usable reply arrived within the engine watchdog. On
    //     GWG only a request that never went out counts (see below).
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
#ifdef VITOHOME_PROTOCOL_GWG
        // GWG has no error telegram: an address the unit does not serve in the
        // requested mode goes unanswered, and that is a TIMEOUT too. Only a
        // request that never went out -- no ENQ from the device -- says
        // anything about the link (GWGEngine::lastTimeoutWasUnsynced()). A
        // request the device took and did not answer proves nothing either way
        // and is ignored here like malformed traffic. Issue #35: consecutive
        // silent `kmbus_eeprom` datapoints published the link offline about 14
        // times in 6.5 minutes on a unit that kept sending ENQs.
        if (this->vito_->lastTimeoutWasUnsynced()) {
          this->link_note_error_();
        } else if (this->vito_->lastSendWasEnqPaced()) {
          // The same two numbers as an answered telegram's `GWG timing:` line,
          // to tell a stale ENQ from a too-early answer to it.
          ESP_LOGD(TAG,
                   "GWG 0x%04X unanswered: enq age <=%" PRIu32 " ms, enq->send %" PRIu32 " ms (delay setting %" PRIu32
                   " ms)",
                   request_address, this->vito_->lastEnqAgeMs(), this->vito_->lastEnqToSendMs(),
                   this->gwg_enq_send_delay_ms_);
        }
#else
        this->link_note_error_();
#endif
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
    if (this->vito_->lastSendWasEnqPaced()) {
      ESP_LOGD(TAG,
               "GWG timing: 0x%04X enq age <=%" PRIu32 " ms, enq->send %" PRIu32 " ms, send->response %" PRIu32 " ms",
               address, enq_age_ms, this->vito_->lastEnqToSendMs(), send_to_response_ms);
    } else {
      ESP_LOGD(TAG, "GWG timing: 0x%04X enq age <=%" PRIu32 " ms (burst), send->response %" PRIu32 " ms", address,
               enq_age_ms, send_to_response_ms);
    }
  });

  // Burst fallback, as a warning: it caps polling at one datapoint per ENQ
  // interval (749-2070 ms on the captures so far) for the rest of the session,
  // and the engine's own warning is compiled out of normal builds (see
  // GWGEngine::OnBurstFallbackCallback).
  this->vito_->onBurstFallback([this](uint8_t consecutive_failures) {
    this->gwg_burst_disabled_ = true;
    ESP_LOGW(TAG,
             "GWG burst disabled after %u unanswered burst-sent request(s); polling is now one "
             "telegram per ENQ for the rest of this session (burst frames %s the leading 0x01; "
             "see set_gwg_burst_omits_enq_ack()).",
             static_cast<unsigned>(consecutive_failures), this->gwg_burst_omits_enq_ack_ ? "omit" : "carry");
  });

  // Late-answer report (2026-11-08, issue #35). WARNING: a TIMEOUT alone does
  // not say whether the device answered too late, too short, or not at all, and
  // the deadline it needs is exactly what set_gwg_response_timeout_ms() tests.
  this->vito_->onLateAnswer([](const optolink::GWGEngine::LateAnswer &late) {
    char hex[3 * sizeof(late.late) + 1] = "";
    for (uint8_t i = 0; i < late.late_stored; ++i)
      snprintf(hex + 3 * i, sizeof(hex) - 3 * i, " %02X", late.late[i]);
    if (late.late_count == 0) {
      ESP_LOGW(TAG, "GWG 0x%04X: %u of %u byte(s) within the %" PRIu32 " ms deadline, nothing after it%s", late.address,
               late.received, late.expected, late.deadline_ms,
               late.retried ? " (burst-sent; retrying on a fresh ENQ)" : "");
      return;
    }
    ESP_LOGW(TAG,
             "GWG 0x%04X: %u of %u byte(s) within the %" PRIu32 " ms deadline; %u late byte(s), the first %" PRIu32
             " ms after the request:%s%s%s",
             late.address, late.received, late.expected, late.deadline_ms, late.late_count, late.first_late_ms, hex,
             late.late_count > late.late_stored ? " ..." : "",
             late.retried ? " (burst-sent; retrying on a fresh ENQ)" : "");
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

#ifdef VITOHOME_PROTOCOL_GWG
  // Hold the main loop awake only while a request waits out the ENQ-to-send
  // delay (default 0: never). start()/stop() are no-ops when already in state.
  if (this->vito_->isHoldingEnq()) {
    this->gwg_hold_loop_.start();
  } else {
    this->gwg_hold_loop_.stop();
  }
#endif

  // Frame logging (compile-time; see vito_uart_interface.h). TX frames are
  // emitted from write(); this closes an RX frame once the bus goes quiet.
  // Compiles to nothing without -DVITOHOME_LOG_FRAMES.
  this->iface_.frame_tick();

  // Start-up protocol verification: confirm the configured protocol actually
  // established a link, or say so with a clear message.
  if (this->protocol_verify_pending_) {
    if (this->link_established_) {
      this->protocol_verify_pending_ = false;
      ESP_LOGI(TAG, "%s link established", PROTOCOL_NAME);
    } else if (static_cast<int32_t>(App.get_loop_component_start_time() - this->protocol_verify_deadline_ms_) >= 0) {
      // Rollover-safe signed-difference comparison. setup() caps the deadline
      // distance at INT32_MAX so this comparison remains valid.
      this->protocol_verify_pending_ = false;

      // Reported, not fatal. This used to mark_failed() the component, which
      // is right for a wrong `protocol:` and wrong for everything else that
      // looks the same from here: a boiler that is switched off or in service
      // when the ESP boots, an Optolink head that is not on yet. Those ended
      // in a dead component until someone rebooted the ESP. The hub keeps
      // polling instead; the link goes online on the first answer
      // (link_note_alive_() clears the error), and identification is repeated
      // then if it had failed.
      ESP_LOGE(TAG,
               "%s link not established; check wiring and that the device speaks this protocol. "
               "Still trying.",
               PROTOCOL_NAME);

      this->publish_link_(false);
      this->status_set_error(LOG_STR("no Optolink link established"));
    }
  }

  // Watchdog: if a request has been in flight too long, surface that and free
  // the hub slot. The protocol engines have their own shorter timeout, so this
  // is a last-resort guard for a lost callback rather than the normal timeout
  // mechanism.
  if (this->in_flight_op_ != OpType::NONE) {
    const uint32_t now = App.get_loop_component_start_time();

    if (now - this->in_flight_started_ms_ > this->in_flight_watchdog_ms_) {
      // A lost engine callback is a link-health signal too.
      this->link_note_error_();

      VitoEntityBase *entity = nullptr;
      const OpType operation = this->take_in_flight_(entity);

      switch (operation) {
        case OpType::IDENT:
          ESP_LOGW(TAG, "Identification read exceeded watchdog (%" PRIu32 " ms)", this->in_flight_watchdog_ms_);
          this->ident_handle_error_();
          break;

        case OpType::RAW:
          ESP_LOGW(TAG, "Raw %s 0x%04X exceeded watchdog (%" PRIu32 " ms)", this->raw_is_write_ ? "write" : "read",
                   this->raw_dp_.address(), this->in_flight_watchdog_ms_);
          this->raw_handle_error_(optolink::OptolinkResult::TIMEOUT);
          break;

        case OpType::READ:
        case OpType::WRITE:
        case OpType::RMW_READ:
          // set_in_flight_() is the only writer of the pair, so an entity
          // operation always carries its entity.
          if (entity == nullptr)
            break;
          ESP_LOGW(TAG, "In-flight %s to %s exceeded watchdog (%" PRIu32 " ms). Clearing.",
                   operation == OpType::WRITE      ? "write"
                   : operation == OpType::RMW_READ ? "read-before-write"
                                                   : "read",
                   entity->get_datapoint().name(), this->in_flight_watchdog_ms_);
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
          } else {
            this->rmw_abort_(entity, optolink::OptolinkResult::TIMEOUT, "read-before-write lost (watchdog)");
          }
          break;

        case OpType::NONE:
          break;
      }
    }
  }

  // Dispatch the next queued request if the bus is idle.
  this->dispatch_next_();
}

void VitoHomeComponent::apply_gwg_diagnostics_() {
#ifdef VITOHOME_PROTOCOL_GWG
  if (!this->vito_)
    return;  // setup() applies them once the engine exists
  this->vito_->setWriteEotBeforePayload(this->gwg_write_eot_before_payload_);
  this->vito_->setBurstOmitsEnqAck(this->gwg_burst_omits_enq_ack_);
  this->vito_->setResponseTimeoutMs(this->gwg_response_timeout_ms_);
  this->vito_->setEnqSendDelayMs(this->gwg_enq_send_delay_ms_);
#endif
}

void VitoHomeComponent::arm_watchdog_(uint8_t telegrams) {
  this->in_flight_started_ms_ = App.get_loop_component_start_time();
  this->in_flight_watchdog_ms_ = in_flight_watchdog_ms(IN_FLIGHT_WATCHDOG_MS, telegrams);
}

void VitoHomeComponent::set_gwg_write_eot_before_payload(bool eot_first) {
  this->gwg_write_eot_before_payload_ = eot_first;
  this->apply_gwg_diagnostics_();
}

void VitoHomeComponent::set_gwg_burst_omits_enq_ack(bool omit) {
  this->gwg_burst_omits_enq_ack_ = omit;
  this->apply_gwg_diagnostics_();
}

void VitoHomeComponent::set_gwg_response_timeout_ms(uint32_t ms) {
  // The upper bound keeps a retried burst read (two deadlines plus an ENQ wait,
  // see GWGEngine::_requestTimeoutMs()) inside one IN_FLIGHT_WATCHDOG_MS budget.
  const uint32_t clamped =
      std::min(std::max(ms, optolink::GWG_RESPONSE_TIMEOUT_MIN_MS), optolink::GWG_RESPONSE_TIMEOUT_MAX_MS);
  this->gwg_response_timeout_ms_ = clamped;
  this->apply_gwg_diagnostics_();
  if (clamped != ms) {
    ESP_LOGW(TAG, "GWG response deadline %" PRIu32 " ms is out of range; using %" PRIu32 " ms", ms, clamped);
  } else {
    ESP_LOGI(TAG, "GWG response deadline set to %" PRIu32 " ms", clamped);
  }
}

void VitoHomeComponent::set_gwg_enq_send_delay_ms(uint32_t ms) {
  const uint32_t clamped = std::min(ms, optolink::GWG_ENQ_SEND_DELAY_MAX_MS);
  this->gwg_enq_send_delay_ms_ = clamped;
  this->apply_gwg_diagnostics_();
  if (clamped != ms) {
    ESP_LOGW(TAG, "GWG ENQ-to-send delay %" PRIu32 " ms is out of range; using %" PRIu32 " ms", ms, clamped);
  } else {
    ESP_LOGI(TAG, "GWG ENQ-to-send delay set to %" PRIu32 " ms", clamped);
  }
}

void VitoHomeComponent::dispatch_next_() {
  if (this->in_flight_op_ != OpType::NONE)
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
    // GWG identification lives in the VIRTUAL access space, not the physical
    // one (2026-08-24). Vitosoft's own GWG tables put the four `SystemIdent`
    // fields at 0xF8..0xFB under FCRead=Virtual_READ, and vcontrold agrees --
    // its getDevType reads 0xF8 len 4 with GETVADDR (0xC7). Reading them on
    // the physical mode returns whatever the physical register file holds at
    // those offsets, which is what produced the unstable identifier this
    // project logged on its GWG capture. Every other protocol reads it with its
    // own read.
    constexpr AddressSpace ident_space = IS_GWG ? AddressSpace::GWG_VIRTUAL : AddressSpace::OWN;
    const bool ident_dispatched = this->send_read_(this->ident_dp_.address(), this->ident_dp_.length(), ident_space);
    if (ident_dispatched) {
      this->set_in_flight_(OpType::IDENT);

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

  // Writes preempt reads.
  if (this->dispatch_write_front_())
    return;

  // Poll/read-back lane. read_queued_ remains true after the item leaves the
  // ring and while its request is in flight. Completion, error, mismatch, or
  // watchdog handling clears it.
  //
  // Address-space affinity: where reaching a space costs a protocol switch
  // (space_has_switch_cost(): over P300 a GFA read detours to KW and back, up
  // to two ENQ waits), the next queued read in the same space goes first, so a
  // run of them shares one switch. The queue holds at most one slot per
  // entity, so this cannot starve the others for longer than one such run.
  if (space_has_switch_cost(this->last_space_)) {
    const AddressSpace space = this->last_space_;
    (void) this->read_queue_.move_first_to_front_if(
        [space](VitoEntityBase *e) { return e != nullptr && e->space() == space; });
  }
  this->dispatch_read_front_();
}

// The two entity lanes hand over the same way as dispatch_raw_front_(): peek at
// the head, give it to the engine, remove it once the engine has accepted the
// telegram. An entity the engine is too busy for stays at the head. No lock is
// held across the hand-off (each RingBuffer call locks only for its own
// duration), so an entity's error handler -- which may publish, and so run
// automations that queue a write -- is called straight from here. The head
// cannot change in between: only the hub's own loop removes from a lane or
// inserts at its head.

bool VitoHomeComponent::dispatch_write_front_() {
  VitoEntityBase *entity = nullptr;
  if (!this->write_queue_.try_front(entity))
    return false;

  if (entity == nullptr) {
    // A null entry violates registration/enqueue invariants. Remove it so it
    // cannot block all later writes.
    ESP_LOGE(TAG, "null entity in write queue; dropping item");
    this->write_queue_.drop_front();
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
      this->write_queue_.drop_front();
      entity->write_queued_ = false;
      entity->handle_write_error(optolink::OptolinkResult::LENGTH);
      return true;
    }
    if (!this->send_read_(wdp.address(), wdp.length(), entity->space())) {
      // Same transient/permanent split as the plain write below.
      if (this->vito_->isBusy())
        return true;  // transient: stays at the head
      ESP_LOGE(TAG, "Engine refused read-before-write for %s (permanent); dropping", entity->get_datapoint().name());
      this->write_queue_.drop_front();
      entity->write_queued_ = false;
      entity->handle_write_error(optolink::OptolinkResult::ERROR);
      return true;
    }
    this->write_queue_.drop_front();
    this->set_in_flight_(OpType::RMW_READ, entity, read_telegram_count(entity->space(), wdp.length()));
    entity->write_queued_ = false;
    entity->write_in_flight_ = true;
    ESP_LOGV(TAG, "Dispatched read-before-write for %s (%u bytes, bit_mask 0x%02X)", entity->get_datapoint().name(),
             wdp.length(), entity->field_mask());
    return true;
  }

  // One space drives both directions (see VitoEntityBase::space()): a GWG
  // entity writes in the mode it polls in, as Vitosoft pairs them
  // (EEPROM_READ, EEPROM_WRITE) / (BE_READ, BE_WRITE). An action
  // (VitoActionButton) is a procedure call, not a write: the same telegram an
  // `access: rpc` read sends, here for its effect.
  const optolink::Datapoint &wdp = entity->get_write_datapoint();
  if (!this->send_write_(wdp.address(), entity->write_data(), entity->write_length(), entity->space(),
                         entity->rpc_prefix(), entity->rpc_prefix_len())) {
    // Busy is transient; anything else is a permanent createPacket() refusal
    // (not reachable with a validated config). A refused write is dropped so
    // the lane cannot spin on it, and the entity is told: an entity waiting
    // for the answer (VitoClock's sync chain) would otherwise wait forever.
    if (this->vito_->isBusy())
      return true;  // transient: stays at the head
    ESP_LOGE(TAG, "Engine refused write for %s (permanent); dropping", entity->get_datapoint().name());
    this->write_queue_.drop_front();
    entity->write_queued_ = false;
    entity->handle_write_error(optolink::OptolinkResult::ERROR);
    return true;
  }

  // The entity has left the queue and is now in flight. Clearing
  // write_queued_ here lets a newer value enqueue while this request waits
  // for its ACK.
  this->write_queue_.drop_front();
  this->set_in_flight_(OpType::WRITE, entity);
  this->last_space_ = entity->space();
  entity->write_queued_ = false;
  entity->write_in_flight_ = true;

  ESP_LOGV(TAG, "Dispatched write for %s (%u bytes)", entity->get_datapoint().name(), entity->write_length());
  return true;
}

void VitoHomeComponent::dispatch_read_front_() {
  VitoEntityBase *entity = nullptr;
  if (!this->read_queue_.try_front(entity))
    return;

  if (entity == nullptr) {
    ESP_LOGE(TAG, "null entity in read queue; dropping item");
    this->read_queue_.drop_front();
    return;
  }

  const optolink::Datapoint &datapoint = entity->get_datapoint();

  // The entity's space picks the telegram (send_read_): a GWG access mode,
  // an `access: rpc` call with its prefix, an `access: gfa` read -- the same
  // lane and the same response path as a read.
  if (!this->send_read_(datapoint.address(), datapoint.length(), entity->space(), entity->rpc_prefix(),
                        entity->rpc_prefix_len())) {
    // See the write lane above: transient _busy retains, a permanent
    // createPacket() rejection (unreachable for schema-validated poll
    // addresses) is dropped so the lane cannot spin. read_queued_ is cleared
    // so the poll scheduler can re-schedule it when next due.
    if (this->vito_->isBusy())
      return;  // transient: stays at the head
    ESP_LOGE(TAG, "Engine refused read for %s (permanent); dropping", entity->get_datapoint().name());
    this->read_queue_.drop_front();
    entity->read_queued_ = false;
    return;
  }

  this->read_queue_.drop_front();
  this->set_in_flight_(OpType::READ, entity, read_telegram_count(entity->space(), datapoint.length()));
  this->last_space_ = entity->space();

  ESP_LOGV(TAG, "Dispatched read for %s", entity->get_datapoint().name());
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

    const PollDecision decision = poll_schedule_step(
        now, entity->next_due_ms_, poll_backoff_interval(entity->poll_interval(), entity->timeout_streak_), slack);

    if (!decision.due)
      continue;

    // Due but not read yet since its last queueing: the backlog this counts.
    // Its schedule stays put, so it is due again on the next cycle.
    if (entity->read_queued_) {
      ++skipped;
      continue;
    }

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
    ESP_LOGW(TAG, "Poll cycle: %zu due entities still queued from an earlier cycle", skipped);
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
    if (this->ident_finder_url_(url, sizeof(url))) {
      ESP_LOGCONFIG(TAG, "  Catalog finder: %s", url);
    }
  }

#ifdef USE_TEXT_SENSOR
  if (this->raw_result_sensor_count_ > 0) {
    ESP_LOGCONFIG(TAG, "  Scan console: %u scan_result sensor(s) attached", this->raw_result_sensor_count_);
  }
#endif

  if (ESPHomeUARTInterface::frame_logging_enabled()) {
    ESP_LOGCONFIG(TAG, "  Frame logging: ON (tag 'vitohome.frames')");
  }
#ifdef VITOHOME_PROTOCOL_GWG
  if (this->gwg_enq_send_delay_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  GWG ENQ-to-send delay: %" PRIu32 " ms", this->gwg_enq_send_delay_ms_);
  }
#endif

  // queue_raw_op()'s gates (raw_access / unsafe_raw_writes).
#ifdef VITOHOME_RAW_UNRESTRICTED
  ESP_LOGCONFIG(TAG, "  Raw access: unrestricted (queue_raw_op)");
#endif
#ifdef VITOHOME_RAW_UNSAFE_WRITES
  ESP_LOGCONFIG(TAG, "  Unsafe raw writes: ON");
#endif

  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup FAILED");
  }

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
// Telegrams per access space
// ---------------------------------------------------------------------------
// The only place an AddressSpace (address_space.h) becomes an engine call, and
// the only protocol #ifdefs on the dispatch path: every lane -- reads, writes,
// read-modify-writes, identification, the raw console -- comes through here. A
// space the protocol has no telegram for is refused like a packet the engine
// will never build (false while the engine is idle), which the callers already
// treat as permanent; the config validation keeps such spaces out of the YAML.

bool VitoHomeComponent::send_read_(uint16_t address, uint8_t length, AddressSpace space, const uint8_t *parameter,
                                   uint8_t parameter_len) {
  if (!space_available(space))
    return false;
  switch (space) {
#ifdef VITOHOME_HAS_RPC
    case AddressSpace::RPC:
      return this->vito_->rpc(address, parameter, parameter_len);
#endif
#ifdef VITOHOME_HAS_GFA
    case AddressSpace::GFA:
      return this->vito_->gfaRead(address, length);
#endif
    default:
      break;
  }
  (void) parameter;
  (void) parameter_len;
#ifdef VITOHOME_PROTOCOL_GWG
  return this->vito_->read(address, length, gwg_mode(space));
#else
  return this->vito_->read(address, length);
#endif
}

bool VitoHomeComponent::send_write_(uint16_t address, const uint8_t *data, uint8_t length, AddressSpace space,
                                    const uint8_t *parameter, uint8_t parameter_len) {
  if (!space_available(space))
    return false;
#ifdef VITOHOME_HAS_RPC
  // An action button: the procedure call is sent for its effect.
  if (space == AddressSpace::RPC)
    return this->vito_->rpc(address, parameter, parameter_len);
#endif
  (void) parameter;
  (void) parameter_len;
  if (!space_writable(space))
    return false;
#ifdef VITOHOME_PROTOCOL_GWG
  return this->vito_->write(address, data, length, gwg_mode(space));
#else
  return this->vito_->write(address, data, length);
#endif
}

bool VitoHomeComponent::send_raw_op_(uint16_t address, uint8_t length, RawLayer layer, uint8_t code,
                                     const uint8_t *payload, uint8_t payload_len) {
  (void) payload_len;  // == length when a payload is given (enqueue_raw_)
  switch (layer) {
#if defined(VITOHOME_PROTOCOL_KW)
    case RawLayer::KW:
      return this->vito_->telegram(code, address, length, payload);
#elif defined(VITOHOME_PROTOCOL_GWG)
    case RawLayer::GWG:
      return this->vito_->telegram(code, address, length, payload);
#else
    case RawLayer::P300:
      return this->vito_->request(code, address, length, payload);
    case RawLayer::KW:
      return this->vito_->kwTelegram(code, address, length, payload);
#endif
    default:
      return false;
  }
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
  VitoEntityBase *entity = nullptr;
  const OpType operation = this->take_in_flight_(entity);

  if (operation == OpType::IDENT) {
    this->ident_handle_response_(response);
    return;
  }

  if (operation == OpType::RAW) {
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

    // The acknowledgement byte of KW and GWG (see raw_handle_response_); P300
    // has none.
    if (response.data != nullptr && response.data_length >= 1) {
      ESP_LOGD(TAG, "Write to %s acknowledged (answered 0x%02X)", entity->get_datapoint().name(), response.data[0]);
    } else {
      ESP_LOGD(TAG, "Write to %s acknowledged", entity->get_datapoint().name());
    }

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
    this->note_poll_answer_(entity);
    if (this->rmw_verify_entity_ == entity)
      this->rmw_verify_read_back_(entity, response);
    entity->handle_response(response);
    this->share_read_response_(entity, response);
    return;
  }

  ESP_LOGW(TAG, "Response received for %s with no valid operation type", entity->get_datapoint().name());
}

// ---------------------------------------------------------------------------
// Read coalescing (see share_read_response_ in vitohome.h)
// ---------------------------------------------------------------------------

VitoEntityBase *VitoHomeComponent::take_waiting_on_same_read_(VitoEntityBase *reader, std::size_t &from) {
  const std::size_t count = this->entities_.size();
  while (from < count) {
    VitoEntityBase *e = this->entities_[from++];
    // read_queued_ covers "in the lane" and "in flight". The one in flight is
    // `reader`, so on any other entity the flag means it sits in the lane.
    // VitoClock (no polling) runs its own state machine on its reads and is
    // left to them.
    if (e == nullptr || e == reader || !e->read_queued_ || !e->wants_polling() || !e->reads_same_datapoint(*reader))
      continue;
    if (!this->read_queue_.remove(e))
      continue;  // flagged but not in the lane: nothing to take
    e->read_queued_ = false;
    return e;
  }
  return nullptr;
}

void VitoHomeComponent::share_read_response_(VitoEntityBase *reader, const ResponseView &response) {
  // One entity at a time, each fully out of the lane before its handler runs:
  // a handler publishes, and an automation on that state may queue a write or
  // a read of its own.
  unsigned shared = 0;
  std::size_t from = 0;
  while (VitoEntityBase *e = this->take_waiting_on_same_read_(reader, from)) {
    this->note_poll_answer_(e);
    // A read-modify-write waiting for its read-back takes this answer as that
    // read-back: any read that completes after the merged write was sent
    // shows the device's state after it.
    if (this->rmw_verify_entity_ == e)
      this->rmw_verify_read_back_(e, response);
    e->handle_response(response);
    ++shared;
  }
  if (shared > 0) {
    ESP_LOGV(TAG, "%s: answer shared with %u more entit%s reading 0x%04X", reader->get_datapoint().name(), shared,
             shared == 1 ? "y" : "ies", reader->get_datapoint().address());
  }
}

void VitoHomeComponent::share_read_error_(VitoEntityBase *reader, optolink::OptolinkResult error) {
  unsigned shared = 0;
  std::size_t from = 0;
  while (VitoEntityBase *e = this->take_waiting_on_same_read_(reader, from)) {
    if (this->rmw_verify_entity_ == e)
      this->rmw_verify_entity_ = nullptr;  // as for the reader: never verify a later poll against it
    if (error == optolink::OptolinkResult::TIMEOUT)
      this->note_poll_timeout_(e);
    e->handle_error(error);
    ++shared;
  }
  if (shared > 0) {
    ESP_LOGD(TAG, "%s: %s also reported to %u more entit%s reading 0x%04X", reader->get_datapoint().name(),
             optolink::errorToString(error), shared, shared == 1 ? "y" : "ies", reader->get_datapoint().address());
  }
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
  const bool was_online = this->link_state_ == 1;
  this->link_established_ = true;
  this->link_error_streak_ = 0;
  this->publish_link_(true);
  // Raised when the start-up window passed without a link (loop()).
  this->status_clear_error();
  // The link came up, for the first time or again. If identification ran
  // while the device was not answering, it ended as "unknown" and would stay
  // that way until a reboot: repeat it now. Only on this edge, so a device
  // that answers but does not serve the identification addresses is asked
  // once per link-up, not continuously. Safe from inside the engine callback
  // that calls this: it only moves the state machine, and the response being
  // delivered belongs to whatever is in flight, which is not identification
  // (its state was DONE).
  if (!was_online && this->identify_device_ && this->ident_state_ == IdentState::DONE &&
      (this->ident_group_ < 0 || this->ident_controller_ < 0)) {
    ESP_LOGI(TAG, "Link is up; repeating the device identification that failed earlier");
    this->ident_start_();
  }
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

void VitoHomeComponent::note_poll_timeout_(VitoEntityBase *entity) {
  // A dead link times every entity out; that is the link-health monitor's
  // business, not a reason to poll anything less. So after the first timeout
  // the streak advances only if the link answered something since this
  // entity's previous one.
  if (entity->timeout_streak_ != 0 && entity->answers_at_timeout_ == this->answers_total_)
    return;
  entity->answers_at_timeout_ = this->answers_total_;
  if (entity->timeout_streak_ < 0xFF)
    ++entity->timeout_streak_;
  const uint32_t interval = entity->poll_interval();
  if (entity->timeout_streak_ == POLL_BACKOFF_AFTER && interval != 0 && interval != POLL_NEVER) {
    ESP_LOGW(TAG,
             "%s: %u timeouts in a row while other datapoints answer; polling it less often (every %" PRIu32
             " s, up to %" PRIu32 "x) until it answers",
             entity->get_datapoint().name(), static_cast<unsigned>(POLL_BACKOFF_AFTER),
             poll_backoff_interval(interval, entity->timeout_streak_) / 1000u, POLL_BACKOFF_MAX_FACTOR);
  }
}

void VitoHomeComponent::note_poll_answer_(VitoEntityBase *entity) {
  if (entity->timeout_streak_ >= POLL_BACKOFF_AFTER) {
    ESP_LOGI(TAG, "%s answers again; polling every %" PRIu32 " s", entity->get_datapoint().name(),
             entity->poll_interval() / 1000u);
  }
  entity->timeout_streak_ = 0;
}

void VitoHomeComponent::on_error_(optolink::OptolinkResult error, uint16_t request_address) {
  VitoEntityBase *entity = nullptr;
  const OpType operation = this->take_in_flight_(entity);

  if (operation == OpType::IDENT) {
    ESP_LOGD(TAG, "Identification read 0x%04X failed (%s)", request_address, optolink::errorToString(error));

    this->ident_handle_error_();
    return;
  }

  if (operation == OpType::RAW) {
    this->raw_handle_error_(error);
    return;
  }

  // Name for the log line: the in-flight entity's datapoint when there is one,
  // else the echoed request address for a stray callback.
  const char *name = entity != nullptr ? entity->get_datapoint().name() : "?";

  switch (error) {
    case optolink::OptolinkResult::TIMEOUT:
      // While the link is known to be offline every request times out, and the
      // hub keeps asking (it no longer gives up when the device is absent at
      // boot). "Optolink link offline" has said it once; repeating it per
      // datapoint at ERROR would fill the log for as long as the device stays
      // away.
      if (this->link_state_ == 0) {
        ESP_LOGV(TAG, "[TIMEOUT] %s (0x%04X) - link offline", name, request_address);
      } else {
        ESP_LOGE(TAG, "[TIMEOUT] %s (0x%04X) - Optolink not responding", name, request_address);
      }
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
    if (error == optolink::OptolinkResult::TIMEOUT)
      this->note_poll_timeout_(entity);
    entity->handle_error(error);
    this->share_read_error_(entity, error);
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

}  // namespace esphome::vitohome
