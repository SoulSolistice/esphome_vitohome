"""Unit tests for scripts/gen_catalog.py against the synthetic fixture.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_gen_catalog.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402

_FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


@pytest.fixture(scope="module")
def catalog():
    return gc.load_catalog(_FIXTURE_DIR)


# --- parsing ----------------------------------------------------------------


def test_devices_discovered(catalog):
    assert "VTestHO1_99" in catalog.devices
    assert "VOther_01" in catalog.devices


def test_events_linked_to_device(catalog):
    events = catalog.events_for("VTestHO1_99")
    assert len(events) == 9
    # The unrelated device has no linked events.
    assert catalog.events_for("VOther_01") == []


def test_addresses_parsed_from_name(catalog):
    events = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    assert events["1"].address == 0x0800
    assert events["2"].address == 0x08A7
    assert events["6"].address == 0x6300


def test_access_types_and_conversions(catalog):
    events = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    assert events["1"].conversion == "Div10"
    assert events["1"].access_type == 1
    assert events["5"].access_type == 2  # writable enum
    assert events["6"].access_type == 2  # writable number


def test_enum_values_attached(catalog):
    events = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    opts = gc._enum_options(events["5"])
    assert (0, "Standby") in opts
    assert (2, "Heizen") in opts


# --- platform routing -------------------------------------------------------


def _platform_of(catalog, event_id, profile="full"):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}[event_id]
    result = gc.emit_entity(ev, profile)
    return result[0] if result else None


def test_routing(catalog):
    assert _platform_of(catalog, "1") == "sensor"  # div10 temp
    assert _platform_of(catalog, "2") == "sensor"  # sec2hour counter
    assert _platform_of(catalog, "3") == "binary_sensor"  # bit field
    assert _platform_of(catalog, "4") == "text_sensor"  # read-only enum
    assert _platform_of(catalog, "5") == "select"  # writable enum
    assert _platform_of(catalog, "6") == "number"  # writable + borders
    assert _platform_of(catalog, "7") == "number"  # writable, no borders
    assert _platform_of(catalog, "8") == "comment"  # DateTimeBCD


def test_bit_mask_from_bit_position(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["3"]
    _platform, lines = gc.emit_entity(ev, "full")
    # BitPosition 2 -> mask 1<<2 = 0x04.
    assert any("bit_mask: 0x04" in ln for ln in lines)


def test_counter_gets_total_increasing_and_slow_poll(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["2"]
    _platform, lines = gc.emit_entity(ev, "full")
    text = "\n".join(lines)
    assert "state_class: total_increasing" in text
    assert f"update_interval: {gc.POLL_SLOW}s" in text


def test_writable_gets_coding_poll(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["6"]
    _platform, lines = gc.emit_entity(ev, "full")
    assert any(f"update_interval: {gc.POLL_CODING}s" in ln for ln in lines)


def test_sec2minute_emits_note(catalog):
    # No preset for Sec2Minute -> noconv + an explicit NOTE comment.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["9"]
    _platform, lines = gc.emit_entity(ev, "full")
    text = "\n".join(lines)
    assert "converter: noconv" in text
    assert "NOTE" in text and "Sec2Minute" in text


# --- profiles & filters -----------------------------------------------------


def test_full_profile_emits_more_than_minimal(catalog):
    full = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    minimal = gc.generate(catalog, "VTestHO1_99", "minimal", None, None)
    assert full.count("- platform: vitohome") >= minimal.count("- platform: vitohome")


def test_minimal_keeps_writables_and_measurements(catalog):
    minimal = gc.generate(catalog, "VTestHO1_99", "minimal", None, None)
    # div10 temp (measurement) and the writable select/number survive minimal.
    assert "0x0800" in minimal  # Outside_Temp
    assert "0x2301" in minimal  # Operating_Mode (writable)
    assert "0x6300" in minimal  # DHW_Setpoint (writable)


def test_include_filter(catalog):
    out = gc.generate(catalog, "VTestHO1_99", "full", r"Temp", None)
    assert "0x0800" in out  # Outside_Temp matches
    assert "0x2301" not in out  # Operating_Mode filtered out


def test_exclude_filter(catalog):
    out = gc.generate(catalog, "VTestHO1_99", "full", None, r"Temp")
    assert "0x0800" not in out
    assert "0x2301" in out


# --- emission shape ---------------------------------------------------------


def test_generate_has_platform_sections(catalog):
    out = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    for section in ("sensor:", "binary_sensor:", "number:", "select:", "text_sensor:"):
        assert section in out
    # All entities are opt-in.
    assert "disabled_by_default: true" in out
    # The DateTimeBCD datapoint shows up only as a commented hint.
    assert "# Error Time @ 0x7507" in out


def test_generated_yaml_parses(catalog):
    yaml = pytest.importorskip("yaml")

    out = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    # No !secret/!include tags in a generated package, so safe_load is fine.
    doc = yaml.safe_load(out)
    assert "sensor" in doc and "select" in doc


# --- _friendly --------------------------------------------------------------


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("Outside_Temp~0x0800", "Outside Temp"),
        ("BurnerHours~0x08A7", "Burner Hours"),
        ("status", "Status"),
    ],
)
def test_friendly(raw, expected):
    assert gc._friendly(raw) == expected


def test_unknown_device_raises(catalog):
    with pytest.raises(SystemExit):
        gc.generate(catalog, "DoesNotExist", "full", None, None)
