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

  // Full only when codegen's upper bound is wrong. FixedVector would drop the
  // push_back silently and the entity would never poll.
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
  // 8E2 near 4800 baud is enforced at config time (FINAL_VALIDATE_SCHEMA in
  // __init__.py), not read back from the bus: see the usb_uart note there.

#ifdef VITOHOME_TIME_SYNC
  // The clock has no platform, so the hub registers it, before the lanes are
  // sized: it takes a read slot, and a write slot when it corrects drift. The
  // platform entities are registered already; the generated setup() body runs
  // before App.setup() reaches this component.
  this->register_entity(&this->clock_);
#endif

  // One allocation per lane, never grown: the read and write lanes hold one
  // slot per registered entity, the raw lane raw_queue_size. A failed
  // allocation fails the component at boot. From here on register_entity()
  // refuses new entities, which the lanes would have no slot for.
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

  // Engine selected at build time (protocol_select.h). Not std::make_unique:
  // without exceptions a failed allocation there aborts the firmware, where
  // this fails soft like the lanes.
  this->vito_.reset(new (std::nothrow) optolink::OptolinkEngine<SelectedProtocol>(&this->iface_));
  if (!this->vito_) {
    ESP_LOGE(TAG, "failed to allocate the protocol engine");
    this->mark_failed(LOG_STR("engine allocation failed"));
    return;
  }
  this->apply_gwg_diagnostics_();

  // `address` is the one in the device's response frame on P300, the request's
  // on KW and GWG. The hub knows what is in flight (in_flight_op_,
  // in_flight_); the address only cross-checks it.
  this->vito_->onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
    this->link_note_alive_();
#ifdef VITOHOME_PROTOCOL_GWG
    this->gwg_note_all_ff_(data, length);
#endif
    ++this->answers_total_;
    this->on_response_(ResponseView{data, length, address}, address);
  });

  this->vito_->onError([this](optolink::OptolinkResult error, uint16_t request_address) {
    // Link health counts silence, not failed operations. NACK and DEVICE_ERROR
    // (a complete, checksum-valid error frame) are answers: they reset the
    // streak and satisfy start-up verification. TIMEOUT advances it. ERROR,
    // CRC and LENGTH are corrupt traffic, possibly line noise, and prove
    // neither.
    switch (error) {
      case optolink::OptolinkResult::NACK:
      case optolink::OptolinkResult::DEVICE_ERROR:
        this->link_note_alive_();
        break;

      case optolink::OptolinkResult::TIMEOUT:
#ifdef VITOHOME_PROTOCOL_GWG
        // GWG has no error telegram: an address the unit does not serve in the
        // requested mode goes unanswered too. Only a request that never went
        // out for want of an ENQ says anything about the link
        // (GWGEngine::lastTimeoutWasUnsynced()); otherwise a run of silent
        // datapoints would publish a unit that keeps sending ENQs offline.
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
  // Timing of every answered read (GWGEngine::OnTimingCallback), at DEBUG on
  // the `vitohome` tag.
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

  // A warning: from here on polling gets one datapoint per ENQ interval (749 to
  // 2070 ms on the captures) until reboot, and the engine's own warning is
  // compiled out of normal builds (GWGEngine::OnBurstFallbackCallback).
  this->vito_->onBurstFallback([this](uint8_t consecutive_failures) {
    this->gwg_burst_disabled_ = true;
    ESP_LOGW(TAG,
             "GWG burst disabled after %u unanswered burst-sent request(s); polling is now one "
             "telegram per ENQ for the rest of this session (burst frames %s the leading 0x01; "
             "see set_gwg_burst_omits_enq_ack()).",
             static_cast<unsigned>(consecutive_failures), this->gwg_burst_omits_enq_ack_ ? "omit" : "carry");
  });

  // A warning: a TIMEOUT alone does not say whether the device answered late,
  // short or not at all, and the deadline it needs is what
  // set_gwg_response_timeout_ms() sets.
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

  // Start-up window for the first answer: max(PROTOCOL_VERIFY_MIN_MS, 3 * hub
  // interval), saturated at INT32_MAX for loop()'s signed-difference deadline
  // check.
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

  this->protocol_verify_deadline_ms_ = millis() + verify_window;

  // Entities are scheduled on hub ticks: a shorter interval runs at the hub's.
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

  // Closes an RX frame log line once the bus is quiet (VITOHOME_LOG_FRAMES,
  // vito_uart_interface.h); nothing otherwise.
  this->iface_.frame_tick();

  if (this->protocol_verify_pending_) {
    if (this->link_established_) {
      this->protocol_verify_pending_ = false;
      ESP_LOGI(TAG, "%s link established", PROTOCOL_NAME);
    } else if (static_cast<int32_t>(App.get_loop_component_start_time() - this->protocol_verify_deadline_ms_) >= 0) {
      this->protocol_verify_pending_ = false;

      // Not fatal: a wrong `protocol:` looks the same from here as a boiler
      // that is off or in service at boot. Polling goes on; the first answer
      // clears the error (link_note_alive_()) and repeats a failed
      // identification.
      ESP_LOGE(TAG,
               "%s link not established; check wiring and that the device speaks this protocol. "
               "Still trying.",
               PROTOCOL_NAME);

      this->publish_link_(false);
      this->status_set_error(LOG_STR("no Optolink link established"));
    }
  }

  // Watchdog for a lost engine callback; the engines time out sooner.
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

  // A merged read-modify-write goes out before anything else: nothing may use
  // the bus between its read and its write (rmw_pending_, vitohome.h).
  if (this->rmw_pending_ != nullptr) {
    this->rmw_dispatch_write_();
    return;
  }

  // Identification goes before regular traffic, so the device shows in the
  // first seconds of the log.
  if (this->ident_state_ != IdentState::IDLE && this->ident_state_ != IdentState::DONE) {
    // On GWG the identification is in the virtual space: Vitosoft's tables put
    // `SystemIdent` 0xF8..0xFB under Virtual_READ, and vcontrold reads 0xF8
    // length 4 with GETVADDR (0xC7). The physical space holds other bytes
    // there.
    constexpr AddressSpace ident_space = IS_GWG ? AddressSpace::GWG_VIRTUAL : AddressSpace::OWN;
    const bool ident_dispatched = this->send_read_(this->ident_dp_.address(), this->ident_dp_.length(), ident_space);
    if (ident_dispatched) {
      this->set_in_flight_(OpType::IDENT);

      ESP_LOGV(TAG, "Dispatched identification read 0x%04X len %u", this->ident_dp_.address(),
               this->ident_dp_.length());
    } else if (!this->vito_->isBusy()) {
      // A permanent refusal (unreachable for these addresses) would spin here
      // with nothing in flight for the watchdog. Treat it as a failed step,
      // as the watchdog would.
      ESP_LOGE(TAG, "Engine refused identification read 0x%04X (permanent); skipping", this->ident_dp_.address());
      this->ident_handle_error_();
    }

    return;
  }

  // The scan console goes before writes and polls: a person is waiting at a
  // button, and the lane is empty unless someone scans.
  if (!this->raw_queue_.empty()) {
    this->dispatch_raw_front_();
    return;
  }

  if (this->dispatch_write_front_())
    return;

  // Where reaching a space costs a protocol switch (space_has_switch_cost():
  // over P300 a GFA read detours to KW and back, up to two ENQ waits), the next
  // queued read in the same space goes first, so a run of them shares one
  // switch. With one slot per entity, the others wait at most one such run.
  if (space_has_switch_cost(this->last_space_)) {
    const AddressSpace space = this->last_space_;
    (void) this->read_queue_.move_first_to_front_if(
        [space](VitoEntityBase *e) { return e != nullptr && e->space() == space; });
  }
  this->dispatch_read_front_();
}

// The entity lanes hand over like dispatch_raw_front_(): peek at the head, give
// it to the engine, remove it once accepted; a busy engine leaves it at the
// head. No lock is held across the hand-off, so an entity's error handler,
// which may publish and run automations that queue a write, is called from
// here. The head cannot change in between: only the hub's loop removes from a
// lane or inserts at its head.

bool VitoHomeComponent::dispatch_write_front_() {
  VitoEntityBase *entity = nullptr;
  if (!this->write_queue_.try_front(entity))
    return false;

  if (entity == nullptr) {
    ESP_LOGE(TAG, "null entity in write queue; dropping item");
    this->write_queue_.drop_front();
    return true;
  }

  if (entity->needs_rmw()) {
    // Read first (RMW_READ) the whole write datapoint, in the entity's space:
    // exactly what the merged write replaces. Config validation rules out a
    // state_address here, so read and write are one register.
    const optolink::Datapoint &wdp = entity->get_write_datapoint();
    if (wdp.length() == 0 || wdp.length() > MAX_RMW_BLOCK) {
      // Unreachable with a validated config; no partial block goes out.
      ESP_LOGE(TAG, "%s: %u-byte read-modify-write exceeds %u bytes; dropping", entity->get_datapoint().name(),
               wdp.length(), MAX_RMW_BLOCK);
      this->write_queue_.drop_front();
      entity->write_queued_ = false;
      entity->handle_write_error(optolink::OptolinkResult::LENGTH);
      return true;
    }
    if (!this->send_read_(wdp.address(), wdp.length(), entity->space())) {
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

  // A GWG entity writes in the mode it polls in, as Vitosoft pairs them
  // (EEPROM_READ/EEPROM_WRITE, BE_READ/BE_WRITE). An action (VitoActionButton)
  // sends the procedure call an `access: rpc` read sends, for its effect.
  const optolink::Datapoint &wdp = entity->get_write_datapoint();
  if (!this->send_write_(wdp.address(), entity->write_data(), entity->write_length(), entity->space(),
                         entity->rpc_prefix(), entity->rpc_prefix_len())) {
    // Busy is transient. Any other refusal is permanent (unreachable with a
    // validated config): the write is dropped, so the lane cannot spin, and
    // the entity is told, so one waiting for the answer (VitoClock) does not
    // wait forever.
    if (this->vito_->isBusy())
      return true;  // transient: stays at the head
    ESP_LOGE(TAG, "Engine refused write for %s (permanent); dropping", entity->get_datapoint().name());
    this->write_queue_.drop_front();
    entity->write_queued_ = false;
    entity->handle_write_error(optolink::OptolinkResult::ERROR);
    return true;
  }

  // write_queued_ clears now, so a newer value can queue while this waits for
  // its ACK.
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

  // The entity's space picks the telegram (send_read_): a GWG access mode, an
  // `access: rpc` call with its prefix, an `access: gfa` read.
  if (!this->send_read_(datapoint.address(), datapoint.length(), entity->space(), entity->rpc_prefix(),
                        entity->rpc_prefix_len())) {
    // As in the write lane; a dropped read is scheduled again when next due.
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
    // Already queued: move that read to the head instead of adding a second
    // one. Otherwise a read-back waits behind the rest of the poll cycle. Not
    // in the lane means its read is in flight; nothing to move.
    static_cast<void>(this->read_queue_.move_to_front(entity));
    return true;
  }

  // Flag first, roll back if the lane refuses, so the entity is never marked
  // queued while absent from the lane.
  entity->read_queued_ = true;

  if (!this->read_queue_.push_front(entity)) {
    entity->read_queued_ = false;
    return false;
  }

  return true;
}

void VitoHomeComponent::schedule_due_entities_() {
  const uint32_t now = App.get_loop_component_start_time();

  // The next due time steps from the previous one, not from `now`
  // (poll_schedule_step()): `now` is a few ms past the tick that called
  // update(), so an interval equal to the hub's, or a multiple, would land just
  // past a tick, fire or not by jitter, and creep. Anything due within half a
  // tick counts as due: the next chance is a whole tick away.
  const uint32_t slack = this->get_update_interval() / 2;

  std::size_t queued = 0;
  std::size_t skipped = 0;
  std::size_t rejected = 0;

  for (auto *entity : this->entities_) {
    if (entity == nullptr)
      continue;

    // VitoClock reads on its own sync schedule, at the head of the lane.
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

    // Flag first, roll back if the lane refuses (as in request_priority_read()).
    entity->read_queued_ = true;

    if (!this->read_queue_.push_back(entity)) {
      entity->read_queued_ = false;
      ++rejected;
      continue;
    }

    // Only a queued read advances the schedule; a refused one is retried next
    // tick.
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
  // The sync schedule runs at the hub's update_interval granularity.
  this->clock_.tick(App.get_loop_component_start_time());
#endif

  if (this->entities_.empty())
    return;

  this->schedule_due_entities_();
}

void VitoHomeComponent::dump_config() {
  // Consecutive lines go out as one ESP_LOGCONFIG, as in ESPHome core: each
  // call site costs flash. The raw lane is the largest fixed allocation, so
  // its capacity is shown.
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

  // No loop over entities_: ESPHome core calls each component's dump_config().
  // The clock is no component, so it is called here. The climate channels are
  // not components either, but have nothing to print.
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
    // Still queued: the staged payload is already the newest, and the queued
    // write sends it.
    return true;
  }

  // Idle, or a write is in flight with its bytes already in the engine's
  // packet: the staged payload is newer and needs a write of its own. Flag
  // first, roll back if the lane refuses.
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
// Where an AddressSpace (address_space.h) becomes an engine call, for every
// lane; the dispatch path's only protocol #ifdefs. A space the protocol has no
// telegram for is refused like a packet the engine never builds (false while
// idle), which the callers treat as permanent. Config validation keeps such
// spaces out of the YAML.

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
// Every completed transaction goes to its owner: identification, the raw
// console, or the entity in flight (VitoClock included).

void VitoHomeComponent::on_response_(const ResponseView &response, uint16_t request_address) {
  VitoEntityBase *entity = nullptr;
  const OpType operation = this->take_in_flight_(entity);

  if (operation == OpType::IDENT) {
    this->ident_handle_response_(response);
    return;
  }

  if (operation == OpType::RAW) {
    // A live check on P300 only; KW and GWG echo the request address.
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

  // A write and an RMW_READ go to the write address, a read to the state
  // address. A live check on P300 only.
  const uint16_t expected_address = (operation == OpType::WRITE || operation == OpType::RMW_READ)
                                        ? entity->get_write_datapoint().address()
                                        : entity->get_datapoint().address();

  if (expected_address != response.address) {
    ESP_LOGW(TAG, "Response address 0x%04X does not match in-flight 0x%04X; dropping", response.address,
             expected_address);

    // A failed operation, as in on_error_(): clear its state only (a newer
    // queued write keeps write_queued_) and tell the entity. VitoClock needs
    // the call to leave its phase, or it never syncs again; for the others a
    // dropped read counts like a timeout. The frame was valid, so the link
    // stays alive.
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
      // Never merge and write back bytes of another address.
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

    // A newer value was queued meanwhile: this ACK is for the older one, while
    // the entity's staged state is the newer. Publishing or reading back now
    // would show a value the device does not have yet; the newer write's ACK
    // does both.
    if (entity->write_queued_) {
      ESP_LOGV(TAG, "Write ACK for %s superseded by a newer queued value", entity->get_datapoint().name());
      if (this->rmw_verify_entity_ == entity)
        this->rmw_verify_entity_ = nullptr;  // the newer write re-reads first
      return;
    }

    entity->handle_write_response(response);
    if (this->rmw_verify_entity_ == entity && !entity->wants_read_back())
      this->rmw_verify_entity_ = nullptr;  // no read-back is coming

    // Siblings first, then the writer's read-back at the head, in front of
    // them: it is the one the verification waits for.
    this->refresh_siblings_(entity);

    if (entity->wants_read_back()) {
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
    // read_queued_ means in the lane or in flight, and `reader` is the one in
    // flight. VitoClock reads for its own state machine and is left out.
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
    // Any read that completes after the merged write shows the state after it,
    // so it serves as that write's read-back.
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
  // millis(), not the loop start time: called from a button or lambda, maybe
  // in another component's loop slot.
  const uint32_t now = millis();

  if (this->last_refresh_all_ms_ != 0 && now - this->last_refresh_all_ms_ < REFRESH_ALL_MIN_INTERVAL_MS) {
    ESP_LOGW(TAG, "refresh_all() suppressed (last one %" PRIu32 " ms ago, min interval %" PRIu32 " ms)",
             now - this->last_refresh_all_ms_, REFRESH_ALL_MIN_INTERVAL_MS);
    return false;
  }

  this->last_refresh_all_ms_ = now;

  // Polled entities only, by the scheduler's own test; VitoClock keeps its
  // own schedule (next_sync_ms_).
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
  // On link-up, repeat an identification that ended unknown while the device
  // was silent. Only on this edge, so a device without the identification
  // addresses is asked once per link-up. Safe inside the engine callback: it
  // only moves the state machine, and what is in flight is not identification
  // (it was DONE).
  if (!was_online && this->identify_device_ && this->ident_state_ == IdentState::DONE &&
      (this->ident_group_ < 0 || this->ident_controller_ < 0)) {
    ESP_LOGI(TAG, "Link is up; repeating the device identification that failed earlier");
    this->ident_start_();
  }
  // The component status mirrors the link too (status LED, HA diagnostics):
  // the connectivity binary_sensor is optional.
  this->status_clear_warning();
}

#ifdef VITOHOME_PROTOCOL_GWG
void VitoHomeComponent::gwg_note_all_ff_(const uint8_t *data, uint8_t length) {
  // GWG has no error telegram: an address the unit does not serve in the
  // requested mode may be answered with 0xFF, which completes the transaction
  // and keeps the link "established". A GWG_VBEM (SW 0x7A) answered 0xFF to all
  // 13 BE-mode addresses its own Vitosoft catalog names.
  //
  // Counts whole responses of 0xFF only, resets on any other, warns once until
  // reboot. Datapoints that really read 0xFF (an unset enum, a missing sensor)
  // trip it only when they are all the config has.
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
    // Cleared by link_note_alive_().
    this->status_set_warning("optolink link offline");
  }
}

void VitoHomeComponent::note_poll_timeout_(VitoEntityBase *entity) {
  // A dead link times every entity out, which is no reason to poll any of them
  // less. After the first timeout the streak advances only if the link answered
  // something since this entity's previous one.
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

  const char *name = entity != nullptr ? entity->get_datapoint().name() : "?";

  switch (error) {
    case optolink::OptolinkResult::TIMEOUT:
      // While the link is offline every request times out and the hub keeps
      // asking. "Optolink link offline" said so once; one ERROR per datapoint
      // would fill the log for as long as the device stays away.
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

    // The device value is unchanged; the entity keeps its state. A newer
    // queued write stays queued.
    entity->handle_write_error(error);
  } else if (operation == OpType::RMW_READ) {
    // Without the current bytes there is nothing to merge into: no blind
    // write.
    this->rmw_abort_(entity, error, "read-before-write failed");
  } else {
    ESP_LOGW(TAG, "Error callback for %s had no valid operation type", name);
  }
}

}  // namespace esphome::vitohome
