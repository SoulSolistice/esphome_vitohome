#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome";

// ---------------------------------------------------------------------------
// Read-modify-write, bit fields and whole blocks (see rmw_pending_ in vitohome.h)
// ---------------------------------------------------------------------------

void VitoHomeComponent::rmw_abort_(VitoEntityBase *entity, optolink::OptolinkResult error, const char *why) {
  ESP_LOGW(TAG, "%s: read-modify-write NOT sent: %s", entity->get_datapoint().name(), why);
  entity->write_in_flight_ = false;
  entity->handle_write_error(error);
}

void VitoHomeComponent::rmw_handle_read_(VitoEntityBase *entity, const ResponseView &response) {
  // A view can carry a length without data; checked before any read.
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
  // An EEPROM FF FF pair means "address not in use" and arrives as 0xFF, like a
  // byte with every flag set. Merging into it and writing it back would put
  // data at an unused address, so the engine flags it
  // (GWGEngine::lastReadHadUnservedEeprom) and the write stops here.
  if (this->vito_->lastReadHadUnservedEeprom()) {
    this->rmw_abort_(entity, optolink::OptolinkResult::ERROR,
                     "GWG EEPROM answered FF FF (address not in use) - refusing to merge into it");
    return;
  }
#endif
  if (!merge_block_field(response.data, block_len, offset, width, entity->field_mask(), entity->write_data(),
                         this->rmw_buf_)) {
    // Unreachable with a validated config: codegen and the entity check the
    // geometry and the value.
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
  const bool write_ok = this->send_write_(wdp.address(), this->rmw_buf_, this->rmw_len_, entity->space());
  if (!write_ok) {
    // Busy: stay armed. dispatch_next_() comes back here first, so nothing
    // else reaches the bus before the merged write.
    if (this->vito_->isBusy())
      return;
    this->rmw_pending_ = nullptr;
    this->rmw_abort_(entity, optolink::OptolinkResult::ERROR, "engine refused the merged write (permanent)");
    return;
  }
  this->rmw_pending_ = nullptr;
  this->set_in_flight_(OpType::WRITE, entity);
  // An ordinary WRITE from here on; its read-back is checked against these
  // bytes.
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
    // Not the writer, which reads back on its own, and not VitoClock, whose
    // state machine takes no unasked answer.
    if (e == nullptr || e == writer || !e->wants_polling() || e->get_datapoint().address() != address)
      continue;
    // Addresses are per space: GWG 0x01 in eeprom and in port, 0x4050 in the
    // Vitotronic and in the burner control, are different registers.
    if (e->space() != writer->space())
      continue;
    // A procedure's answer is computed, not read from the written memory.
    if (e->is_rpc())
      continue;
    if (this->request_priority_read(e))
      ++queued;
  }
  if (queued > 0) {
    ESP_LOGD(TAG, "%s: refreshing %u entit%s sharing 0x%04X", writer->get_datapoint().name(), queued,
             queued == 1 ? "y" : "ies", address);
  }
}

}  // namespace esphome::vitohome
