"""Bit fields (BitLength 1..8) in scripts/gen_catalog.py.

A bit field shares its byte with other datapoints. Before `bit_mask`, a
writable one was emitted as a whole-byte number/select/switch: choosing
"1 ohne Nachlauf" on GWG_K13Speichernachlauf (EEPROM 0x0005) wrote 0x01 and
cleared the other seven coding flags in that byte. Now every bit field except
a read-only single bit goes through _emit_bit_field(), which emits a native
`bit_mask` entity; the component reads the byte first and writes back only the
field (read-modify-write, vitohome.h rmw_pending_).

Pinned here: the MSB-first mask derivation, the writable-shape gate (a 1-byte
datapoint, or block mode for a field in a wider block), the read-only fallbacks
(inferred scale, MultOffset with an offset, a block with a read-only sibling,
--no-partial-writes), enum handling in field units, and the issue #21 bytes
decoding to the export's factory defaults.

    python -m pytest tests/unit/test_gen_catalog_multibit.py -q
"""

import os
import re
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog.constants import PROTO_GWG, PROTO_P300  # noqa: E402
from vitocatalog.emitter import emit_entity  # noqa: E402
from vitocatalog.models import Event, EventValue  # noqa: E402
from vitocatalog.transforms import _bit_field  # noqa: E402


def _ev(values=None, **over):
    """GWG EEPROM 4-bit field shaped like GWG_BEMK89SchaltpunktSPR30Loben
    (export 0.0.26.4683), including its stale 0.5/-16 factor pair."""
    kw = {
        "id": "t",
        "name": "GWG BEMK89SchaltpunktSPR30Loben",
        "tech": "GWG_BEMK89SchaltpunktSPR30Loben",
        "address": 0x001E,
        "conversion": "NoConversion",
        "access_type": 3,
        "block_length": 1,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 4,
        "bit_position": 0,
        "lower": "0",
        "upper": "1",
        "stepping": "0.1",
        "fc_read": "EEPROM_READ",
        "fc_write": "EEPROM_WRITE",
        "conv_factor": 0.5,
        "conv_offset": -16.0,
    }
    kw.update(over)
    ev = Event(**kw)
    if values:
        ev.values = [EventValue(enum_address_value=v, description=label) for v, label in values]
    return ev


def _flag(**over):
    """GWG_K13Speichernachlauf: one of the eight coding flags at EEPROM 0x0005."""
    kw = {
        "name": "GWG K13Speichernachlauf",
        "tech": "GWG_K13Speichernachlauf",
        "address": 0x0005,
        "bit_length": 1,
        "bit_position": 4,
        "lower": "",
        "upper": "",
        "stepping": "",
        "conv_factor": 2.0,
        "conv_offset": 6.0,
    }
    kw.update(over)
    return _ev(values=[(0, "0 mit Nachlauf bis 10 Min."), (1, "1 ohne Nachlauf")], **kw)


def _key(lines, key):
    for ln in lines:
        s = ln.strip()
        if s.startswith(f"{key}:"):
            return s.split(":", 1)[1].split("#", 1)[0].strip()
    return None


def _mask(lines):
    v = _key(lines, "bit_mask")
    return None if v is None else int(v, 16)


def _filters(lines):
    out = []
    for ln in lines:
        m = re.match(r"\s*- (multiply|offset): (\S+)$", ln)
        if m:
            out.append((m.group(1), float(m.group(2))))
    return out


_SCALE = {"noconv": 1.0, "div2": 0.5, "div10": 0.1, "div100": 0.01, "mult2": 2.0, "mult5": 5.0, "mult10": 10.0}


def _decode(lines, raw_byte):
    """What the component publishes: mask/shift, converter scale, then filters."""
    mask = _mask(lines)
    shift = (mask & -mask).bit_length() - 1
    v = float((raw_byte & mask) >> shift) * _SCALE[_key(lines, "converter")]
    for name, arg in _filters(lines):
        v = v * arg if name == "multiply" else v + arg
    return v


# --- mask derivation ------------------------------------------------------------


@pytest.mark.parametrize(
    "bit_position,bit_length,expected",
    [
        (0, 4, (0, 4, 0x0F)),  # SPR30Loben / K06: high nibble (MSB-first)
        (4, 4, (0, 0, 0x0F)),  # SPR30Lunten / K05: low nibble
        (3, 4, (0, 1, 0x0F)),  # GWG_BEMKC2VerstaerkungsfaktorRaum
        (0, 3, (0, 5, 0x07)),  # GWG_Uhrzeit_Zehner_Minuten_BEM
        (3, 5, (0, 0, 0x1F)),  # GWG_Uhrzeit_Stunden_BEM
        (116, 4, (14, 0, 0x0F)),  # nvoAlarm_Anlagennummer: byte 14 of the block
        (4, 1, (0, 3, 0x01)),  # K13Speichernachlauf: mask 0x08
        (7, 1, (0, 0, 0x01)),  # Neptun_*: mask 0x01
        (61, 4, None),  # WPR3_Geraetestatus_Meldung_anstehend: straddles bytes
        (7, 4, None),  # KBUS_Anlage_Kesselfolge_Wahlschalter: straddles bytes
        (104, 16, None),  # nviAlarm_Kennung: two bytes
    ],
)
def test_bit_field(bit_position, bit_length, expected):
    assert _bit_field(_ev(bit_position=bit_position, bit_length=bit_length)) == expected


def test_mask_line_cites_the_vitosoft_numbering():
    _, lines = emit_entity(_ev(), "full", PROTO_GWG)
    line = next(ln for ln in lines if ln.strip().startswith("bit_mask:"))
    assert "bit_mask: 0xF0" in line
    assert "BitPosition 0, BitLength 4" in line


# --- the 0x0005 bug class: writable flags ---------------------------------------


def test_writable_flag_is_a_bit_mask_select_not_a_whole_byte_write():
    platform, lines = emit_entity(_flag(), "full", PROTO_GWG)
    assert platform == "select"
    assert _mask(lines) == 0x08
    assert _key(lines, "address") == "0x0005"
    assert _key(lines, "length") == "1"
    text = "\n".join(lines)
    # options are FIELD values, not byte values
    assert '0: "0 mit Nachlauf bis 10 Min."' in text and '1: "1 ohne Nachlauf"' in text
    assert "state_address" not in text and "byte_offset" not in text


def test_boolean_flag_becomes_a_bit_mask_switch():
    # GWG_K27Ruecklaufsensor-shaped: on/off labels -> native toggle.
    ev = _flag(name="GWG K27Ruecklaufsensor", tech="GWG_K27Ruecklaufsensor", address=0x0006, bit_position=0)
    ev.values = [
        EventValue(enum_address_value=0, description="Aus"),
        EventValue(enum_address_value=1, description="Ein"),
    ]
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "switch"
    assert _mask(lines) == 0x80


def test_multi_bit_enum_select_options_in_field_units():
    # GWG_K08NeigungKessel: options 0..15 in the high nibble of 0x0002.
    ev = _flag(
        name="GWG K08NeigungKessel",
        tech="GWG_K08NeigungKessel",
        address=0x0002,
        bit_position=0,
        bit_length=4,
    )
    ev.values = [EventValue(enum_address_value=i, description=f"{0.2 + i / 10:.1f}") for i in range(16)]
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "select"
    assert _mask(lines) == 0xF0
    assert sum(1 for ln in lines if re.match(r"\s+\d+: ", ln)) == 16


def _sm1_status(values):
    # SM1_Sensor_7_Status_GWG (VScotHO1_72): 7-bit field (mask 0x7F) whose own
    # value list includes 128 = "Nicht verwendet" -- the sibling 0x80 flag.
    return _ev(
        values=values,
        address=0xCF90,
        block_length=20,
        byte_length=20,
        bit_position=33,
        bit_length=7,
        access_type=1,
        fc_read="Virtual_READ",
        fc_write="",
        lower="",
        upper="",
        stepping="",
    )


def test_enum_whose_options_contradict_its_bit_length_is_a_comment():
    # Hardware, 2026-10-14, no SM1 module installed: the byte reads 0x82. With
    # 128 dropped and mask 0x7F applied, the entity published "Unterbrechung"
    # (open circuit) for a sensor that does not exist. The 0x80 bit is
    # SM1_Sensor_7_Status_Verwendet ("1: nicht verwendet").
    ev = _sm1_status([(0, "Sensor ok"), (1, "Kurzschluss"), (2, "Unterbrechung"), (128, "Nicht verwendet")])
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "comment"
    assert "128" in lines[0]
    assert "value list and BitLength disagree" in lines[0]


def test_enum_whose_options_fit_is_still_emitted():
    platform, lines = emit_entity(
        _sm1_status([(0, "Sensor ok"), (1, "Kurzschluss"), (2, "Unterbrechung")]), "full", PROTO_P300
    )
    assert platform == "text_sensor"
    assert _mask(lines) == 0x7F
    assert _key(lines, "byte_offset") == "4" and _key(lines, "length") == "20"


# --- numeric fields -----------------------------------------------------------------


def test_noconversion_numeric_field_is_writable_with_stale_factor_ignored():
    # GWG_BEMKB6DAnteilRegelungDLH: 0..15 / 1 carrying a stale 5/35 pair.
    ev = _ev(address=0x0022, lower="0", upper="15", stepping="1", conv_factor=5.0, conv_offset=35.0)
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "number"
    assert _mask(lines) == 0xF0
    assert _key(lines, "converter") == "noconv"
    assert (_key(lines, "min_value"), _key(lines, "max_value"), _key(lines, "step")) == ("0", "15", "1")
    assert "factor/offset (5, 35) ignored" in "\n".join(lines)
    assert _filters(lines) == []


def test_number_without_borders_gets_the_field_range():
    ev = _ev(address=0x0022, lower="", upper="", stepping="", conv_factor=None, conv_offset=None)
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "number"
    assert (_key(lines, "min_value"), _key(lines, "max_value"), _key(lines, "step")) == ("0", "15", "1")


@pytest.mark.parametrize(
    "raw_byte,value",
    [(0x87, 0.8), (0x37, 0.3), (0xA0, 1.0)],
)
def test_spr30_inferred_scale_stays_read_only(raw_byte, value):
    # 0..1 / 0.1 is an inferred encoding (patch 0001's rule): read-only and
    # UNVERIFIED even though bit-field writes are on. Factory 0x87 -> 0,8 (ALZ).
    platform, lines = emit_entity(_ev(), "full", PROTO_GWG)
    assert platform == "sensor"
    assert _mask(lines) == 0xF0
    assert _decode(lines, raw_byte) == pytest.approx(value)
    assert "UNVERIFIED" in "\n".join(lines)


@pytest.mark.parametrize("bit_position,value", [(0, 85.0), (4, 75.0)])
def test_multoffset_with_offset_is_read_only_and_decodes_issue_21(bit_position, value):
    # 0x0001 reads 0xA8 on 0x2053 HW 0x01 SW 0x7A (issue #21): K06 = 85 degC,
    # K05 = 75 degC -- both the export's factory defaults (ALZ).
    ev = _ev(
        address=0x0001,
        conversion="MultOffset",
        bit_position=bit_position,
        lower="35",
        upper="110",
        stepping="5",
        conv_factor=5.0,
        conv_offset=35.0,
    )
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "sensor"
    assert _decode(lines, 0xA8) == pytest.approx(value)
    assert "no encode path" in "\n".join(lines)


def test_multoffset_preset_factor_is_writable_with_the_converter():
    ev = _ev(conversion="MultOffset", lower="0", upper="150", stepping="10", conv_factor=10.0, conv_offset=0.0)
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "number"
    assert _key(lines, "converter") == "mult10"
    assert _filters(lines) == []


# --- the writable-shape gate -----------------------------------------------------------


def _neptun():
    # Neptun_Interne_Pumpe (VScotHO1_72): bit 0x01 of byte 0 of a 2-byte block.
    ev = _flag(
        name="Neptun Interne Pumpe",
        tech="Neptun_Interne_Pumpe",
        address=0x7951,
        block_length=2,
        byte_length=1,
        bit_position=7,
        fc_read="Virtual_READ",
        fc_write="Virtual_WRITE",
    )
    ev.values = [
        EventValue(enum_address_value=0, description="Aus"),
        EventValue(enum_address_value=1, description="Ein"),
    ]
    return ev


def test_bit_field_in_a_wider_block_is_a_block_write():
    # Block mode: the whole 2-byte block at the base, byte_offset 0 (which is
    # what selects block mode), the bit. Byte 1 (Drehzahl) is written back as read.
    platform, lines = emit_entity(_neptun(), "full", PROTO_P300)
    assert platform == "switch"
    assert (_key(lines, "address"), _key(lines, "length"), _key(lines, "byte_offset")) == ("0x7951", "2", "0")
    assert _mask(lines) == 0x01
    assert "read-modify-write of the whole 2-byte block" in "\n".join(lines)


def test_bit_field_in_a_block_with_a_read_only_sibling_stays_read_only():
    platform, lines = emit_entity(_neptun(), "full", PROTO_P300, ro_blocks=frozenset({(0x7951, "")}))
    assert platform == "binary_sensor"
    assert _mask(lines) == 0x01
    assert "read-only (live) data" in "\n".join(lines)


def test_interior_field_reads_the_block_base():
    ev = _ev(
        address=0xA132,
        block_length=29,
        byte_length=29,
        bit_position=116,
        lower="",
        upper="",
        stepping="",
        conv_factor=None,
        conv_offset=None,
        access_type=1,
        fc_read="Virtual_READ",
        fc_write="",
    )
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "sensor"
    assert (_key(lines, "address"), _key(lines, "length"), _key(lines, "byte_offset")) == ("0xA132", "29", "14")
    assert _mask(lines) == 0x0F


def test_no_partial_writes_keeps_everything_read_only():
    platform, lines = emit_entity(_flag(), "full", PROTO_GWG, partial_writes=False)
    assert platform == "binary_sensor"
    assert "--no-partial-writes" in "\n".join(lines)
    ev = _ev(address=0x0022, lower="0", upper="15", stepping="1")
    platform, _ = emit_entity(ev, "full", PROTO_GWG, partial_writes=False)
    assert platform == "sensor"


def test_field_spanning_bytes_is_a_comment():
    platform, lines = emit_entity(_ev(bit_position=61, block_length=10), "full", PROTO_GWG)
    assert platform == "comment"
    assert "does not fit in one byte" in lines[0]


def test_contradictory_byte_position_is_a_comment():
    # WW_Temperatur_Mitte_ab_Bit_4 (0x01CF): BitPosition 4 -> byte 0, BytePosition 2
    ev = _ev(bit_position=4, byte_position=2, block_length=3, access_type=1, fc_read="Virtual_READ", fc_write="")
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "comment"
    assert "contradictory" in lines[0]


def test_read_only_single_bit_stays_on_the_binary_sensor_path():
    ev = _ev(bit_length=1, bit_position=2, access_type=1, lower="", upper="", stepping="", fc_write="")
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "binary_sensor"
    assert "  bit_mask: 0x20" in lines


def test_generated_bit_field_yaml_is_valid():
    yaml = pytest.importorskip("yaml")
    for ev in (_ev(), _flag(), _ev(address=0x0022, lower="0", upper="15", stepping="1")):
        _, lines = emit_entity(ev, "full", PROTO_GWG)
        doc = yaml.safe_load("\n".join(lines))
        assert doc[0]["bit_mask"] in (0xF0, 0x08)
