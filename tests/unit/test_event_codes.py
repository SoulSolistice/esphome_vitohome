"""Unit tests for the event platform's fault-code handling (event.py).

Focus: the 0x00 slot-empty code. VitoEvent::handle_response short-circuits on
0x00 and fires the built-in "cleared" event type, so 0x00 must never reach
``event_types`` or ``add_code()`` -- but it must still VALIDATE, because the
fault-code map is legitimately shared (as a YAML anchor) with the
``error_history`` text_sensor, where 0x00 IS looked up. See
example/vscotho1_72.dp.curated.yaml, which defines the map once in the
text_sensor and reuses it here via ``codes: *vd300_codes``.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402

from components.vitohome.event import CONFIG_SCHEMA  # noqa: E402


@pytest.fixture(autouse=True)
def _fresh_core():
    CORE.reset()
    yield
    CORE.reset()


_NAME_SEQ = iter(range(1000))


def _cfg(codes, **extra):
    base = {"name": f"Fault {next(_NAME_SEQ)}", "address": 0x7507, "codes": codes}
    base.update(extra)
    return CONFIG_SCHEMA(base)


# --- validation ------------------------------------------------------------


def test_zero_code_validates_so_the_shared_anchor_keeps_working():
    # Rejecting 0x00 here would force the ~200-entry error_history map to be
    # duplicated just to drop one key.
    cfg = _cfg({0x00: "Regelbetrieb (kein Fehler)", 0x10: "Kurzschluss"})
    assert 0x00 in cfg["codes"]


def test_code_wider_than_one_byte_is_rejected():
    with pytest.raises(cv.Invalid, match="does not fit one byte"):
        _cfg({0x100: "too wide"})


def test_empty_code_map_is_rejected():
    with pytest.raises(cv.Invalid, match="at least one fault code"):
        _cfg({})


# --- codegen filter --------------------------------------------------------
#
# to_code() is async and needs a full codegen context, so assert against the
# same expression it uses rather than driving cg. Keep these in lockstep with
# event.py::to_code.


def _emitted(codes):
    """Mirror of event.py::to_code's 0x00 filter."""
    fault_codes = {code: label for code, label in codes.items() if code != 0x00}
    event_types = [f"0x{code:02X}" for code in fault_codes]
    event_types += ["cleared", "unknown"]
    return fault_codes, event_types


def test_zero_code_is_filtered_out_of_event_types_and_add_code():
    codes = {0x00: "Regelbetrieb (kein Fehler)", 0x10: "Kurzschluss", 0x18: "Unterbrechung"}
    fault_codes, event_types = _emitted(codes)
    assert 0x00 not in fault_codes
    assert "0x00" not in event_types
    assert event_types == ["0x10", "0x18", "cleared", "unknown"]


def test_event_types_use_uppercase_hex_matching_the_cpp_trigger():
    # vito_event.cpp formats with "0x%02X"; a case mismatch would make every
    # fired event an unregistered type.
    _, event_types = _emitted({0xAB: "Fehler"})
    assert event_types[0] == "0xAB"


def test_all_zero_map_still_yields_the_builtin_types():
    fault_codes, event_types = _emitted({0x00: "kein Fehler"})
    assert fault_codes == {}
    assert event_types == ["cleared", "unknown"]
