#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome";

// Names for the identification at 0xF8/0xF9, for the log: only units seen on
// the wire; the catalog finder does the matching. `hw` is the hardware index
// (0xFA), negative when unread; only GWG needs it.
static const char *ident_family_name(uint16_t ident, int hw = -1) {
  switch (ident) {
    case 0x20CB:
      return "VScotHO1";
    case 0x2098:
      return "V200KW2";
    case 0x2094:
      return "V200KW1";
    case 0x2053:
      // Four GWG families share 0x2053. Vitosoft's ecnDataPointType rows tell
      // them apart by IdentificationExtension's high byte, the hardware index:
      // 0x01 VBEM, 0x02 VBES, 0x08 VWMS, 0x10 VBT2 (the low byte is the
      // software index).
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

// Burner-control chip names from Vitosoft's table (eventvaluetype
// Kennung_FA_Chip~<n>: "24h - NEC"), for the log. The byte at 0x7650
// (GFA_Kennung) selects the GFA fault texts
// (example/catalogs/<lang>/gfa/chip_<XX>.yaml). VScotHO1_72 reads 0x24.
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

// Catalog finder (web/, on GitHub Pages). The identification bytes go in the
// query string; the page matches them against example/catalogs/resolve.json.
// The device never fetches it.
static const char *const CATALOG_FINDER_URL = "https://soulsolistice.github.io/esphome_vitohome/";

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

  // dispatch_next_() sends it when the bus is free.
}

void VitoHomeComponent::ident_handle_response_(const ResponseView &response) {
  // A live check on P300 only; KW and GWG echo the request address.
  if (response.address != this->ident_dp_.address()) {
    ESP_LOGW(TAG, "Identification response address 0x%04X does not match in-flight 0x%04X", response.address,
             this->ident_dp_.address());
    this->ident_handle_error_();
    return;
  }

  const uint8_t *data = response.data;
  const uint8_t length = response.data_length;

  if (length != 0 && data == nullptr) {
    ESP_LOGW(TAG, "Identification response has length %u but no payload", length);
    this->ident_handle_error_();
    return;
  }

  switch (this->ident_state_) {
    case IdentState::READ4:
      // Register order: F8 group, F9 controller, FA hardware index, FB
      // software index.
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
      if (length >= 1 && data[0] != 0x00 && data[0] != 0xFF) {
        this->ident_gfa_ = data[0];
      } else {
        ESP_LOGD(TAG, "No burner-control chip at 0x7650");
      }
      this->ident_dispatch_(IdentState::READ_F0);
      return;

    case IdentState::READ_F0:
      // Kept raw, in register order; the catalog finder decodes it (Vitosoft:
      // one little-endian Int) and decides what it selects. FF FF is KW's
      // answer for an address the unit lacks; 00 00 is a value (the generic
      // variant of a family F0 subdivides).
      if (length >= 2 && !(data[0] == 0xFF && data[1] == 0xFF)) {
        this->ident_f0_ = (data[0] << 8) | data[1];
      } else {
        ESP_LOGD(TAG, "No extended identification (F0) at 0x00F0");
      }
      this->ident_finish_();
      return;

    default:
      return;
  }
}

void VitoHomeComponent::ident_handle_error_() {
  // Each step fails soft: the 4-byte read falls back to single-byte reads, and
  // a failed single-byte read leaves its field unknown.
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

    // Each field on its own, as in ident_finder_url_().
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
  if (this->ident_finder_url_(url, sizeof(url))) {
    ESP_LOGI(TAG, "Find the matching datapoint catalog: %s", url);
  }

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
