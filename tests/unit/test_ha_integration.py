"""Unit tests for the HA-integration round: fault-event platform schema,
connectivity binary_sensor schema, and the catalog generator's
unit -> device_class mapping."""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
for _p in (_REPO_ROOT, os.path.join(_REPO_ROOT, "scripts")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402
from gen_catalog import _device_class_for, _unit_for  # noqa: E402

from components.vitohome.binary_sensor import CONFIG_SCHEMA as BS_SCHEMA  # noqa: E402
from components.vitohome.event import CONFIG_SCHEMA as EVENT_SCHEMA  # noqa: E402


@pytest.fixture(autouse=True)
def _fresh_core():
    # ESPHome registers validated entity names in CORE for uniqueness; see
    # tests/unit/test_switch.py for the background.
    CORE.reset()
    yield
    CORE.reset()


_NAME_SEQ = iter(range(1000))


def _name():
    return f"Test Entity {next(_NAME_SEQ)}"


# --- event platform -----------------------------------------------------------


def test_event_minimal_validates():
    cfg = EVENT_SCHEMA({"name": _name(), "address": 0x7507, "codes": {0x10: "Aussensensor"}})
    assert cfg["length"] == 9  # full FA01 slot by default
    assert cfg["codes"] == {0x10: "Aussensensor"}


def test_event_codes_required():
    with pytest.raises(cv.Invalid):
        EVENT_SCHEMA({"name": _name(), "address": 0x7507})


def test_event_empty_codes_rejected():
    with pytest.raises(cv.Invalid, match="at least one fault code"):
        EVENT_SCHEMA({"name": _name(), "address": 0x7507, "codes": {}})


def test_event_code_must_fit_one_byte():
    with pytest.raises(cv.Invalid, match="does not fit one byte"):
        EVENT_SCHEMA({"name": _name(), "address": 0x7507, "codes": {0x1FF: "zu gross"}})


def test_event_length_range():
    cfg = EVENT_SCHEMA({"name": _name(), "address": 0x7507, "length": 1, "codes": {0x10: "x"}})
    assert cfg["length"] == 1
    with pytest.raises(cv.Invalid):
        EVENT_SCHEMA({"name": _name(), "address": 0x7507, "length": 10, "codes": {0x10: "x"}})


# --- connectivity binary_sensor -------------------------------------------------


def test_connectivity_needs_no_address():
    cfg = BS_SCHEMA({"type": "connectivity", "name": _name()})
    assert str(cfg["device_class"]) == "connectivity"
    assert str(cfg["entity_category"]) == "diagnostic"


def test_connectivity_rejects_address():
    with pytest.raises(cv.Invalid):
        BS_SCHEMA({"type": "connectivity", "name": _name(), "address": 0x0800})


def test_datapoint_type_is_default_and_still_needs_address():
    cfg = BS_SCHEMA({"name": _name(), "address": 0x0883, "bit_mask": 0x01})
    assert cfg["address"] == 0x0883
    with pytest.raises(cv.Invalid):
        BS_SCHEMA({"name": _name(), "bit_mask": 0x01})


def test_binary_sensor_accepts_block_interior_bit():
    # HK_Frostgefahr_aktivA1M1: byte 16 of the 22-byte block at 0x2500. The
    # block is read at its base (aligned; P300 NAKs an interior read) and the
    # bit is indexed inside the payload.
    cfg = BS_SCHEMA(
        {"name": _name(), "address": 0x2500, "length": 22, "byte_offset": 16, "bit_mask": 0x01},
    )
    assert cfg["length"] == 22
    assert cfg["byte_offset"] == 16


def test_binary_sensor_block_read_capped_at_one_telegram():
    with pytest.raises(cv.Invalid):
        BS_SCHEMA({"name": _name(), "address": 0x2500, "length": 64, "byte_offset": 16})


def test_binary_sensor_block_read_without_byte_offset_is_allowed():
    # A bit in byte 0 of a wide block: the generator omits byte_offset when it
    # is 0, so `length` alone must be accepted up to the telegram cap.
    cfg = BS_SCHEMA({"name": _name(), "address": 0x1410, "length": 10, "bit_mask": 0x01})
    assert cfg["length"] == 10
    assert cfg["byte_offset"] == 0


def test_binary_sensor_offset_must_lie_inside_block():
    with pytest.raises(cv.Invalid):
        BS_SCHEMA({"name": _name(), "address": 0x2500, "length": 22, "byte_offset": 22})


# --- gen_catalog device_class map ----------------------------------------------


def test_device_class_mapping_grounded_units():
    assert _device_class_for("\u00b0C") == "temperature"
    assert _device_class_for("kWh") == "energy"
    assert _device_class_for("Wh") == "energy"
    assert _device_class_for("mbar") == "pressure"
    assert _device_class_for("dBm") == "signal_strength"
    assert _device_class_for("h") == "duration"
    assert _device_class_for("min") == "duration"
    assert _device_class_for("s") == "duration"


def test_unmapped_units_get_no_device_class():
    # %, K deltas and free-text units must not be force-classified.
    for unit in ("%", "K", "Prozent pro K", "l pro h", "Minuten", "months", "sech", ""):
        assert _device_class_for(unit) is None


def test_unit_for_normalizes_mbar():
    from types import SimpleNamespace

    ev = SimpleNamespace(values=[], unit="mBar")
    assert _unit_for(ev) == "mbar"


# --- error_history fault-code key range (Audit SS4.5) ----------------------
# The error_history codes map is keyed by the decoded wire code BYTE, so keys
# must fit 0..0xFF -- parity with event.py::_validate_codes. A wider key is a
# dead entry that can never match the 8-bit code. text_sensor.py validates this
# via the _validate_code_bytes post-validator on the error_history schema (a
# key-marker that raises is swallowed by voluptuous into a generic "extra keys
# not allowed" error, hence the post-validator).

from components.vitohome.text_sensor import CONFIG_SCHEMA as TS_SCHEMA  # noqa: E402


def test_error_history_accepts_byte_codes():
    cfg = TS_SCHEMA(
        {
            "type": "error_history",
            "name": _name(),
            "address": 0x7507,
            "codes": {0x00: "kein Fehler", 0xFF: "max"},
        }
    )
    assert cfg["type"] == "error_history"


def test_error_history_rejects_out_of_range_code():
    with pytest.raises(cv.Invalid, match="does not fit one byte"):
        TS_SCHEMA(
            {
                "type": "error_history",
                "name": _name(),
                "address": 0x7507,
                "codes": {0x100: "zu gross"},
            }
        )


def test_error_history_codes_optional():
    # codes: is optional (defaults to empty); a bare error_history is valid.
    cfg = TS_SCHEMA(
        {
            "type": "error_history",
            "name": _name(),
            "address": 0x7507,
        }
    )
    assert cfg["codes"] == {}
