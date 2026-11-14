"""Generator fixes from issue #35 (Vitodens 200 WB2, GWG 0x2053, gwg_vbem_03).

* The GWG timers shipped as their raw counter: the "Rohwert" twin at the same
  field was linked first, and the field de-duplication keeps the first event.
* GWG_Timer1Min shipped unconverted: its MultOffset pair -0.1067 / 6992.58 is
  the export's placeholder pair everywhere else, and was dropped as one.
* Scaled sensors showed at ESPHome's default of 0 decimals (Wasserdruck in
  whole bar, a div2 temperature 35.5 as 36).
* A *HighByteFirst row was decoded little-endian.

    python -m pytest tests/unit/test_gen_catalog_issue35.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog.constants import COUNTER, PLAIN, PROTO_GWG, PROTO_P300  # noqa: E402
from vitocatalog.emitter import _accuracy_decimals, emit_entity  # noqa: E402
from vitocatalog.loader import Catalog  # noqa: E402
from vitocatalog.models import Event  # noqa: E402
from vitocatalog.renderer import generate  # noqa: E402


def _ev(**over):
    kw = {
        "id": "1",
        "name": "Timer",
        "tech": "GWG_Timer13Min",
        "token": "GWG_Timer13Min~0x0039",
        "address": 0x0039,
        "conversion": "NoConversion",
        "access_type": 1,
        "block_length": 2,
        "byte_length": 2,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "0",
        "upper": "65535",
        "stepping": "1",
        "unit": "",
        "fc_read": "Physical_READ",
        "fc_write": "",
        "parameter_type": "Int",
    }
    kw.update(over)
    return Event(**kw)


def _catalog(events):
    cat = Catalog(
        types={"GWG_TEST": "dp1"},
        dp_name={"dp1": "GWG Test"},
        links={"dp1": [e.id for e in events]},
        raw_events={},
        vlinks={},
        raw_values={},
        access={},
        ident_rows=[
            {"token": "GWG_TEST", "ident": 0x2053, "ext": None, "extt": None, "f0": None, "f0t": None, "error_type": "1"}
        ],
        textmap={},
    )
    cat._events = {e.id: e for e in events}
    return cat


def _entity_block(out: str, address: int) -> str:
    """The emitted entity whose `address:` line names ``address``."""
    for chunk in out.split("  - platform: vitohome"):
        if f"address: 0x{address:04X}" in chunk:
            return chunk
    raise AssertionError(f"0x{address:04X} not emitted")


# --- the converted datapoint wins over its Rohwert twin ---------------------


def test_converted_timer_wins_over_its_rohwert_twin():
    # Linked raw first, as in the export: the de-duplication used to keep it.
    raw = _ev(id="1", name="Timer 13 min Rohwert", tech="GWG_Timer13MinRohwert", token="GWG_Timer13MinRohwert~0x0039")
    minutes = _ev(
        id="2",
        name="Timer 13 min",
        conversion="MultOffset",
        conv_factor=0.000223,
        conv_offset=0.0,
        upper="14.6",
        stepping="0.1",
        unit="min",
    )
    out = generate(_catalog([raw, minutes]), "GWG_TEST", "full", None, None, emit_device_id=False)
    block = _entity_block(out, 0x0039)
    assert '"Timer 13 min"' in block
    assert "Rohwert" not in block
    assert "multiply: 0.000223" in block


def test_order_is_unchanged_without_a_rohwert_row():
    a = _ev(id="1", name="First", tech="GWG_A", token="GWG_A~0x0039")
    b = _ev(id="2", name="Second", tech="GWG_B", token="GWG_B~0x0039")
    out = generate(_catalog([a, b]), "GWG_TEST", "full", None, None, emit_device_id=False)
    assert '"First"' in _entity_block(out, 0x0039)


# --- MultOffset keeps the factor pair the placeholder shares ---------------


def _loaded(conversion: str):
    raw_events = {
        "1": {
            "Address": "GWG_Timer1Min~0x0035",
            "Name": "GWG_Timer1Min",
            "Type": "1",
            "Conversion": conversion,
            "ConversionFactor": "-0.1067",
            "ConversionOffset": "6992.58",
            "BlockLength": "2",
            "ByteLength": "2",
            "BytePosition": "0",
        }
    }
    cat = Catalog({"GWG_TEST": "dp1"}, {"dp1": "GWG Test"}, {"dp1": ["1"]}, raw_events, {}, {}, {}, [], {})
    return cat.events_for("GWG_TEST")[0]


def test_multoffset_keeps_the_placeholder_shaped_pair():
    ev = _loaded("MultOffset")
    assert ev.conv_factor == pytest.approx(-0.1067)
    assert ev.conv_offset == pytest.approx(6992.58)
    # Raw 64972 from the issue log reads 60.07 min against the row's 0..60.
    assert 64972 * ev.conv_factor + ev.conv_offset == pytest.approx(60.07, abs=0.01)


def test_the_placeholder_is_still_dropped_on_other_conversions():
    ev = _loaded("NoConversion")
    assert ev.conv_factor is None
    assert ev.conv_offset is None


# --- accuracy_decimals from the decode -------------------------------------


@pytest.mark.parametrize(
    ("conv", "filters", "expected"),
    [
        ("noconv", [], None),
        ("mult10", [], None),
        ("div2", [], 1),
        ("div10", [], 1),
        ("div100", [], 2),
        ("div1000", [], 3),
        ("noconv", [("multiply", 0.0052), ("offset", -0.65)], 3),  # Wasserdruck
        ("noconv", [("multiply", -0.1067), ("offset", 6992.58)], 2),  # Timer1Min
        ("noconv", [("multiply", 0.5), ("offset", -16.0)], 1),
        ("noconv", [("multiply", 0.000001)], 3),  # capped
        ("noconv", [("offset", 0.25)], 2),
    ],
)
def test_accuracy_decimals(conv, filters, expected):
    assert _accuracy_decimals(conv, filters, PLAIN) == expected


def test_counter_keeps_one_decimal():
    assert _accuracy_decimals("sec2hour", [], COUNTER) == 1


def test_div2_sensor_emits_accuracy_decimals():
    ev = _ev(conversion="Div2", block_length=1, byte_length=1, upper="127.5", stepping="0.5", unit="°C")
    _platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert "accuracy_decimals: 1" in "\n".join(lines)


def test_raw_sensor_has_no_accuracy_decimals_line():
    _platform, lines = emit_entity(_ev(), "full", PROTO_GWG)
    assert "accuracy_decimals" not in "\n".join(lines)


# --- *HighByteFirst is big-endian ------------------------------------------


def _hbf(**over):
    # vcLanModemReset~0xAE01-shaped: writable 2-byte minutes, IntHighByteFirst.
    kw = {
        "name": "Zyklischer Modemreset",
        "tech": "vcLanModemReset",
        "token": "vcLanModemReset~0xAE01",
        "address": 0xAE01,
        "access_type": 3,
        "upper": "255",
        "unit": "min",
        "fc_write": "Physical_WRITE",
        "parameter_type": "IntHighByteFirst",
    }
    kw.update(over)
    return _ev(**kw)


def test_unsigned_high_byte_first_word_is_rotatebytes_read_only():
    platform, lines = emit_entity(_hbf(), "full", PROTO_P300)
    text = "\n".join(lines)
    assert platform == "sensor"
    assert "converter: rotatebytes" in text
    assert "big-endian" in text


@pytest.mark.parametrize(
    "over",
    [
        {"parameter_type": "SIntHighByteFirst"},  # signed: rotatebytes is unsigned
        {"conversion": "Div10"},  # scaled: rotatebytes has no scale
        {"conversion": "MultOffset", "conv_factor": 0.5, "conv_offset": 0.0},
        {"block_length": 3, "byte_length": 3},  # rotatebytes is 2 bytes only
        {"block_length": 4, "byte_length": 4},
    ],
)
def test_other_high_byte_first_rows_are_a_hint(over):
    platform, lines = emit_entity(_hbf(**over), "full", PROTO_P300)
    assert platform == "comment"
    assert "big-endian" in "\n".join(lines)


def test_one_byte_high_byte_first_is_untouched():
    platform, lines = emit_entity(_hbf(block_length=1, byte_length=1, access_type=1, fc_write=""), "full", PROTO_P300)
    assert platform == "sensor"
    assert "converter: noconv" in "\n".join(lines)
