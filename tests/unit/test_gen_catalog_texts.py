"""Vitosoft texts in scripts/gen_catalog.py: languages, units, fault texts, names.

The full Textresource_<culture>.xml files of a Vitosoft installation carry every
entity name, option label and fault text (35,640 ``viessmann.errorcode.*`` labels
per language). Pinned here:

* the text layers (slim file < German < chosen language) and that an empty
  translation never hides a German text;
* units come from scripts/units.py, never from the translated label (Russian
  "кВтч", German "-dBm", English empty strings);
* the fault-text index and its fallback order (device set, generic set for
  ErrorType 1 only, vd300 only for exports without fault texts);
* the GFA burner-control texts keyed by the chip byte read at 0x7650;
* entity names cleaned and made unique per platform, as ESPHome requires;
* the generator's own names per language.

    python -m pytest tests/unit/test_gen_catalog_texts.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import fault_codes  # noqa: E402
import units  # noqa: E402
from vitocatalog.constants import CULTURES, PROTO_P300  # noqa: E402
from vitocatalog.emitter import emit_entity  # noqa: E402
from vitocatalog.loader import Catalog, _load_text_layers, _lookup_label, _resolve_unit  # noqa: E402
from vitocatalog.models import Event, EventValue  # noqa: E402
from vitocatalog.renderer import _clean_name, _make_names_unique, _share_codes_blocks  # noqa: E402
from vitocatalog.special import _error_history_entries, _error_history_lines, _resolve_fault_codes, _resolve_gfa_codes  # noqa: E402
from vitocatalog.transforms import _boolean_pair, _dedup_option_labels, _device_class_for, _enum_bool_pair, _enum_options  # noqa: E402

# --- units.py ------------------------------------------------------------------------------


def test_every_unit_row_is_well_formed():
    for token, row in units.UNITS.items():
        assert isinstance(row, tuple) and len(row) == 3, token
        symbol, device_class, negate = row
        assert isinstance(symbol, str) and symbol == symbol.strip(), token
        assert device_class is None or isinstance(device_class, str), token
        assert isinstance(negate, bool), token


def test_one_device_class_per_symbol():
    # DEVICE_CLASS_BY_SYMBOL is keyed by symbol: two rows giving one symbol two
    # different classes would make the result depend on table order.
    classes: dict = {}
    for symbol, device_class, _neg in units.UNITS.values():
        if device_class:
            assert classes.setdefault(symbol, device_class) == device_class, symbol


def test_units_never_come_from_the_translation():
    russian = {"ecnUnit.kWh": "кВтч", "ecnUnit.dBm": "#NAME?"}
    assert _resolve_unit("ecnUnit.kWh", russian) == "kWh"
    assert _resolve_unit("ecnUnit.dBm", russian) == "dBm"
    assert _resolve_unit("ecnUnit.U pro min", {}) == "rpm"
    assert _resolve_unit("ecnUnit.Sekunden", {}) == "s"


def test_unknown_unit_token_falls_back_to_german_then_the_token():
    assert _resolve_unit("ecnUnit.Neu", {"ecnUnit.Neu": "N/x"}) == "N/x"
    assert _resolve_unit("ecnUnit.Neu", {}) == "Neu"


def test_device_classes_follow_the_table():
    assert _device_class_for("kWh") == "energy"
    assert _device_class_for("W") == "power"
    assert _device_class_for("m³/h") == "volume_flow_rate"
    for unit in ("K", "%", "m³", "L/h", "%/K", ""):
        assert _device_class_for(unit) is None, unit


# --- text layers -------------------------------------------------------------------------------


def _textresource(path, cultures, rows):
    """A minimal Textresource file: rows are (culture_id, label, value)."""
    head = "".join(f'<Culture Id="{cid}" CompanyId="24" Name="{name}" />' for cid, name in cultures)
    body = "".join(
        f'<TextResource CompanyId="24" CultureId="{cid}" Label="{lab}" Value="{val}" />' for cid, lab, val in rows
    )
    xml = (
        '<?xml version="1.0" encoding="utf-16"?>'
        '<DocumentElement xmlns="http://viessmann.com/Textresource.xsd">'
        f"<Cultures>{head}</Cultures><TextResources>{body}</TextResources></DocumentElement>"
    )
    path.write_bytes(xml.encode("utf-16"))


def test_layers_prefer_the_language_and_fall_back_to_german_per_label(tmp_path):
    _textresource(
        tmp_path / "Textresource.xml", [(1, "de"), (2, "en")], [(1, "ui.only", "Nur DE"), (2, "ui.only", "Only EN")]
    )
    _textresource(
        tmp_path / "Textresource_de.xml",
        [(1, "de")],
        [(1, "a", "Kesseltemperatur"), (1, "b", "Brenner"), (1, "c", "nur deutsch")],
    )
    _textresource(tmp_path / "Textresource_en.xml", [(2, "en")], [(2, "a", "Boiler temperature"), (2, "b", "")])
    textmap, german = _load_text_layers(str(tmp_path), "en")
    assert textmap["a"] == "Boiler temperature"
    assert textmap["b"] == "Brenner"  # an empty translation never hides German
    assert textmap["c"] == "nur deutsch"  # missing in English -> German
    assert textmap["ui.only"] == "Only EN"  # slim file, culture preferred
    assert german["a"] == "Kesseltemperatur" and german["ui.only"] == "Nur DE"
    de_map, _ = _load_text_layers(str(tmp_path), "de")
    assert de_map["a"] == "Kesseltemperatur"


def test_every_vitosoft_culture_is_selectable():
    assert len(CULTURES) == 18
    assert {"de": "1", "en": "2", "sv": "14", "no": "15", "sk": "16", "hr": "17", "ro": "18"}.items() <= (CULTURES.items())


# --- fault texts -----------------------------------------------------------------------------------


class _Cat:
    """Just enough of Catalog for the fault-text resolution."""

    fault_texts = Catalog.fault_texts

    def __init__(self, text, error_types=None):
        self._text = text
        self._fault_index = None
        self._error_types = error_types or {}

    def identification_for(self, token):
        return {"error_type": self._error_types.get(token)}


_TEXTS = {
    "viessmann.errorcode.VScotHO1_72.10": "Kurzschluss Außentemperatursensor",
    "viessmann.errorcode.VScotHO1_72.F4": "keine Flammenbildung ",
    "viessmann.errorcode.10": "Kurzschluss Außentemperatursensor (generisch)",
    "viessmann.errorcode.fa.24.0A": "10: Flammenausfall in der Sicherheitszeit",
    "viessmann.errorcode.fa.41.0A": "10: anderer Chip",
    "viessmann.errorcode.SMS.Text": "not a code",
    "viessmann.errorcode.X.100": "wider than a byte",
    "viessmann.errorcode.VScotHO1_72.00": "",
}


def test_fault_index_splits_device_generic_and_gfa():
    per_device, generic, gfa = _Cat(_TEXTS).fault_texts()
    assert per_device["VScotHO1_72"] == {0x10: "Kurzschluss Außentemperatursensor", 0xF4: "keine Flammenbildung"}
    assert generic == {0x10: "Kurzschluss Außentemperatursensor (generisch)"}
    assert gfa["24"] == {0x0A: "10: Flammenausfall in der Sicherheitszeit"}
    assert "X" not in per_device  # 0x100 does not fit the code byte


def test_device_set_wins():
    codes, note = _resolve_fault_codes(_Cat(_TEXTS), "VScotHO1_72", True, "auto")
    assert codes[0x10] == "Kurzschluss Außentemperatursensor"
    assert "for this device" in note


def test_generic_set_only_for_errortype_1():
    cat = _Cat(_TEXTS, {"VCaldens_HO1": "1", "VBlockEM18-36": "4"})
    codes, note = _resolve_fault_codes(cat, "VCaldens_HO1", True, "auto")
    assert codes == {0x10: "Kurzschluss Außentemperatursensor (generisch)"} and "generic" in note
    # A BHKW is not a Vitotronic boiler: no map rather than the boiler meaning.
    assert _resolve_fault_codes(cat, "VBlockEM18-36", True, "auto") == (None, "")


def test_vd300_only_when_the_export_has_no_fault_texts():
    codes, note = _resolve_fault_codes(_Cat({}), "VScotHO1_72", True, "auto")
    assert codes is fault_codes.SETS["vd300"] and "no fault texts" in note
    assert _resolve_fault_codes(_Cat({}), "VScotHO1_72", True, "vitosoft") == (None, "")


def test_explicit_curated_set_and_codes_off():
    codes, _ = _resolve_fault_codes(_Cat(_TEXTS), "VScotHO1_72", True, "vd200")
    assert codes is fault_codes.SETS["vd200"]
    assert _resolve_fault_codes(_Cat(_TEXTS), "VScotHO1_72", False, "auto") == (None, "")


@pytest.mark.parametrize("chip", ["24", "0x24", "0X24"])
def test_gfa_chip_selects_its_set(chip):
    codes, note = _resolve_gfa_codes(_Cat(_TEXTS), chip)
    assert codes == {0x0A: "10: Flammenausfall in der Sicherheitszeit"} and "chip 24" in note


def test_gfa_without_chip_says_how_and_unknown_chip_fails():
    codes, note = _resolve_gfa_codes(_Cat(_TEXTS), None)
    assert codes is None and "GFA=0x<XX> the hub logs at boot" in note
    with pytest.raises(SystemExit, match="known: 24, 41"):
        _resolve_gfa_codes(_Cat(_TEXTS), "99")


def test_gfa_slots_get_the_chip_texts_and_system_slots_do_not():
    gfa_entry = {"address": 0x7590, "name": "GFA Fehler 01", "disabled": True, "system": False, "gfa": True, "comment": []}
    text = "\n".join(_error_history_lines(gfa_entry, "gfa_fehler_01", {0x10: "F-code"}, "", {0x0A: "10: x"}, "GFA note"))
    assert '0x0A: "10: x"' in text and "F-code" not in text and "# GFA note" in text


# --- names -----------------------------------------------------------------------------------------


def test_clean_name_trims_and_replaces_the_url_separator():
    assert _clean_name("  Aktiver Anlagen- / Solltemperaturwert  ") == "Aktiver Anlagen- ⁄ Solltemperaturwert"


def test_duplicate_names_get_their_ids_case_insensitively():
    def chunk(name, oid):
        return ((0,), "", ["  - platform: vitohome", f'    name: "{name}"', f"    id: {oid}"])

    buckets = {
        "sensor": [chunk("Pegel", "RF_0_Pegel"), chunk("pegel", "RF_1_Pegel"), chunk("Einzeln", "Solo")],
        "text_sensor": [chunk("Pegel", "Other")],
    }
    assert _make_names_unique(buckets) == 2
    names = [c[2][1].strip() for c in buckets["sensor"]]
    assert names == ['name: "Pegel (RF_0_Pegel)"', 'name: "pegel (RF_1_Pegel)"', 'name: "Einzeln"']
    # Uniqueness is per platform: the text_sensor of the same name is untouched.
    assert buckets["text_sensor"][0][2][1].strip() == 'name: "Pegel"'


def test_generator_names_per_language():
    ev = Event(
        id="e",
        name="err",
        address=0x7507,
        conversion="NoConversion",
        access_type=1,
        block_length=90,
        byte_length=90,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        token="ecnsysEventType~Error",
        block_factor=10,
    )
    assert [e["name"] for e in _error_history_entries(ev)[:2]] == ["Letzter Fehler", "Fehler 02"]
    assert [e["name"] for e in _error_history_entries(ev, "en")[:2]] == ["Last fault", "Fault 02"]
    assert _error_history_entries(ev, "fr")[0]["name"] == "Last fault"  # other languages: English
    # ids never change with the language
    assert [e["seed"] for e in _error_history_entries(ev, "en")[:2]] == ["letzter_fehler", "fehler_02"]


# --- dBm sign -------------------------------------------------------------------------------------


def test_dbm_magnitude_is_negated():
    ev = Event(
        id="p",
        name="Pegel",
        tech="RF_TeilnehmerInfo_1_Pegel",
        token="RF_TeilnehmerInfo_1_Pegel~0x0D08",
        address=0x0D08,
        conversion="NoConversion",
        access_type=1,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        unit="dBm",
        unit_token="ecnUnit.dBm",
        fc_read="Virtual_READ",
    )
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    text = "\n".join(lines)
    assert platform == "sensor"
    assert 'unit_of_measurement: "dBm"' in text and "device_class: signal_strength" in text
    assert "- multiply: -1" in text


# --- option labels ---------------------------------------------------------------------------------


def test_option_label_resolves_without_the_name_segment():
    # DPDefinitions: @@viessmann.eventvaluetype.name.X~0; Textresource: viessmann.eventvaluetype.X~0
    text = {"viessmann.eventvaluetype.ExtBetriebsartenumschaltung_M2~0": "OFF"}
    assert _lookup_label("@@viessmann.eventvaluetype.name.ExtBetriebsartenumschaltung_M2~0", text) == "OFF"
    assert _lookup_label("@@viessmann.eventvaluetype.name.Missing~0", text) == ""
    assert _lookup_label("plain", text) == ""


def _enum(values):
    ev = Event(
        id="s",
        name="Sel",
        address=0x3549,
        conversion="NoConversion",
        access_type=3,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
    )
    ev.values = [EventValue(enum_address_value=v, description=d, label=shown, label_de=g) for v, d, shown, g in values]
    return ev


def test_option_labels_per_language():
    # (value, Description, label in the catalog language, German UI label)
    ev = _enum([(0, "AUS", "OFF", "Aus"), (1, "EIN", "ON", "Ein"), (2, "", "", "Automatik")])
    assert _enum_options(ev) == [(0, "OFF"), (1, "ON"), (2, "Automatik")]  # missing English -> German
    assert _enum_options(ev, german=True) == [(0, "AUS"), (1, "EIN"), (2, "Automatik")]
    # A German catalog carries no `label` (Catalog._mk_value): Description first,
    # the German UI label only where Description is empty.
    de = _enum([(0, "AUS", "", "Aus"), (1, "", "", "Ein")])
    assert _enum_options(de) == [(0, "AUS"), (1, "Ein")]


def test_switch_decision_does_not_depend_on_the_language():
    # French labels _boolean_pair cannot read; the German ones decide, the
    # French ones are shown.
    ev = _enum([(0, "AUS", "Arrêt", "Aus"), (1, "EIN", "Marche", "Ein")])
    shown = _dedup_option_labels(_enum_options(ev))
    assert _boolean_pair(shown) is None
    assert _enum_bool_pair(ev, shown) == (1, 0, "Marche", "Arrêt")


# --- shared codes blocks ---------------------------------------------------------------------


def _slot(oid, codes):
    lines = ["  - platform: vitohome", "    type: error_history", f"    id: {oid}", "    # note", "    codes:"]
    return lines + [f'      0x{c:02X}: "{t}"' for c, t in codes] + ["    update_interval: 300s"]


def test_repeated_codes_maps_become_aliases_in_output_order():
    anchors: dict = {}
    first = _share_codes_blocks(_slot("letzter_fehler", [(0, "a"), (1, "b")]), anchors)
    second = _share_codes_blocks(_slot("fehler_02", [(0, "a"), (1, "b")]), anchors)
    other = _share_codes_blocks(_slot("gfa_fehler_01", [(0, "x")]), anchors)
    assert "    codes: &letzter_fehler_codes" in first and '      0x01: "b"' in first
    assert "    codes: *letzter_fehler_codes  # same map as above" in second
    assert not any(ln.startswith("      0x") for ln in second)  # the rows are gone
    assert second[-1] == "    update_interval: 300s"  # what follows the map stays
    assert "    codes: &gfa_fehler_01_codes" in other  # a different map anchors itself


def test_chunk_without_codes_is_untouched():
    chunk = ["  - platform: vitohome", "    id: plain"]
    assert _share_codes_blocks(chunk, {}) == chunk
