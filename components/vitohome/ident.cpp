#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome";

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
      if (length >= 1 && data[0] != 0x00 && data[0] != 0xFF) {
        this->ident_gfa_ = data[0];
      } else {
        ESP_LOGD(TAG, "No burner-control chip at 0x7650");
      }
      this->ident_dispatch_(IdentState::READ_F0);
      return;

    case IdentState::READ_F0:
      // Raw and uninterpreted: reading F0 from the two bytes (Vitosoft: one
      // little-endian Int) and what it selects is the catalog finder's
      // business. FF FF is KW's
      // answer for an address the unit does not have; 00 00 is a real value
      // (the generic variant of a family F0 subdivides).
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
