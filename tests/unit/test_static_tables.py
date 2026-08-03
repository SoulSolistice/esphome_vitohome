"""Unit tests for the static-table emitters that replaced the per-row add_*() runs.

The contract that matters at config time is the empty-input guard: a zero-length
C++ array (``static const VitoOption x[] = {};``) does not compile, so an empty
mapping must yield no table at all and the caller must skip the setter.

The other half of this work (the emitted C++ shape) is not expressible here --
it is verified by reading the generated ``main.cpp``, since a wrong-but-valid
Python object still passes config validation. That is exactly how the
``EnumValue`` mode bug was caught: ``cv.enum`` yields a str subclass whose
``str()`` is the YAML key, so interpolating it emitted a bare ``heat`` instead of
``climate::CLIMATE_MODE_HEAT`` -- valid Python, undeclared C++.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from esphome.core import CORE  # noqa: E402

from components.vitohome import (  # noqa: E402
    HUB_LINK_SENSORS,
    cpp_string_literal,
    emit_option_table,
    emit_uint32_table,
    register_hub_sensor,
)


@pytest.fixture(autouse=True)
def _fresh_core():
    CORE.reset()
    yield
    CORE.reset()


class _FakeID(str):
    """Stand-in for the entity ID; the emitters only interpolate it into a name."""

    __slots__ = ()


_CONFIG = {"id": _FakeID("vitohome_ts_1")}


# --- the empty-input guard (the case that would emit invalid C++) -----------


def test_empty_option_mapping_emits_no_table():
    assert emit_option_table(_CONFIG, {}, "options") == (None, 0)


def test_empty_uint32_list_emits_no_table():
    assert emit_uint32_table(_CONFIG, [], "raw_values") == (None, 0)


def test_empty_tuple_is_handled():
    # `raw` text_sensors reach the emitter with an empty mapping, and event.py's
    # 0x00 filter can empty a one-entry map.
    assert emit_option_table(_CONFIG, {}, "codes") == (None, 0)
    assert emit_uint32_table(_CONFIG, (), "on_values") == (None, 0)


# --- counts -----------------------------------------------------------------


def test_option_table_count_matches_mapping():
    _table, count = emit_option_table(_CONFIG, {0x10: "a", 0x18: "b", 0x20: "c"}, "options")
    assert count == 3


def test_uint32_table_count_matches_values():
    _table, count = emit_uint32_table(_CONFIG, [0, 1, 2, 175], "raw_values")
    assert count == 4


# --- label escaping ---------------------------------------------------------
#
# Labels are interpolated into the generated C++ table, so a quote or backslash
# in a YAML label must not break the literal. The emitter routes them through
# cpp_string_literal, the same helper datapoint_expression uses.


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        ("Normal", '"Normal"'),
        ('say "hi"', '"say \\"hi\\""'),
        ("back\\slash", '"back\\\\slash"'),
        ("Überhitzung", '"Überhitzung"'),
    ],
)
def test_labels_are_escaped_for_cpp(raw, expected):
    assert cpp_string_literal(raw) == expected


# --- hub sensor collection (M4) ---------------------------------------------
#
# The collection lives in CORE.data rather than a module global specifically so
# that CORE.reset() clears it. A module global would leak entities between
# configs compiled in one process -- which is what this test suite does.


def test_hub_sensor_collection_is_reset_with_core():
    register_hub_sensor(HUB_LINK_SENSORS, "sensor_a")
    assert CORE.data["vitohome"]["hub_sensors"][HUB_LINK_SENSORS] == ["sensor_a"]
    CORE.reset()
    assert "vitohome" not in CORE.data


def test_hub_sensor_collection_accumulates_in_order():
    for name in ("a", "b", "c"):
        register_hub_sensor(HUB_LINK_SENSORS, name)
    assert CORE.data["vitohome"]["hub_sensors"][HUB_LINK_SENSORS] == ["a", "b", "c"]
