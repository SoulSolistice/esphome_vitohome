"""Regression tests for the multi-byte writable-enum (select) fix in
``scripts/gen_catalog.py``.

Before the fix, ``emit_entity()`` emitted a ``select`` only for **1-byte**
writable enums (``length == 1``); a 2-byte writable enum fell through to the
``number`` branch and lost its Home Assistant dropdown (degrading to a pinned
``0/0/1`` placeholder when it had no borders). The vitohome ``select`` platform
(``components/vitohome/select.py``) accepts ``length`` 1-2
(``validate_length_in(1, 2)``) and ``vito_select.cpp`` encodes / reads back the
configured width, so a 2-byte enum can and should be a ``select``.

These tests feed ``emit_entity()`` a structurally-typed stand-in
(``types.SimpleNamespace``) instead of a fixture row, so they exercise the
function's attribute contract directly without needing the binary Vitosoft
export. The stand-in sets *every* attribute ``emit_entity()`` may read on any
branch, so attribute access never raises regardless of the path taken.

Run::

    python -m pytest tests/unit/test_gen_catalog_multibyte_select.py -q
"""

import os
import sys
from types import SimpleNamespace

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402


def _enum_value(raw, label):
    """Stand-in for ``gen_catalog.EventValue`` carrying the fields that
    ``_enum_options()`` / ``_unit_for()`` read."""
    return SimpleNamespace(
        enum_address_value=raw,
        enum_replace_value=label,
        name=label,
        description="",
        unit="",
        lower="",
        upper="",
        stepping="",
    )


def _event(**overrides):
    """Stand-in for ``gen_catalog.Event`` with every attribute ``emit_entity()``
    may touch on any branch. ``conversion=""`` deterministically routes through
    ``CONVERSION_MAP``'s default (raw ``noconv``); the tests assert nothing
    about the resulting NOTE line."""
    base = dict(
        id="1",
        name="Test Enum",
        address=0x2500,
        conversion="",
        access_type=2,  # writable (2/3)
        block_length=0,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        lower="",
        upper="",
        stepping="",
        enum_type=True,
        unit="",
        tech="test_enum",
        token="Test~0x2500",
        fc_read="",  # "" -> reachable
        fc_write="",  # "" -> writable (trusted to access_type)
        values=[],
    )
    base.update(overrides)
    return SimpleNamespace(**base)


def test_two_byte_writable_enum_emits_select():
    # Non-boolean labels: a semantic on/off pair would (correctly) become a
    # switch since the boolean-pair heuristic; "Stufe 1/2" is a choice and
    # must keep exercising the select branch this file guards.
    ev = _event(
        byte_length=2,
        values=[_enum_value(0x0000, "Stufe 1"), _enum_value(0x0100, "Stufe 2")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "select"
    assert "  length: 2" in body
    # Option keys are zero-padded to the field width (2 bytes -> 4 hex digits).
    assert "    0x0000: " in body
    assert "    0x0100: " in body


def test_two_byte_boolean_enum_emits_switch():
    # The switch twin: an EIN/AUS pair on a 2-byte field emits a switch with
    # the same width handling (values zero-padded to 4 hex digits when
    # non-default).
    ev = _event(
        byte_length=2,
        values=[_enum_value(0x0000, "AUS"), _enum_value(0x0100, "EIN")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "switch"
    assert "  length: 2" in body
    assert "on_value: 0x0100" in body
    assert "off_value: 0x0000" in body


def test_one_byte_writable_enum_unchanged_format():
    ev = _event(
        byte_length=1,
        values=[_enum_value(0x00, "Standby"), _enum_value(0x02, "Heizen")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "select"
    assert "  length: 1" in body
    # 1-byte output stays byte-for-byte identical to the old `0x{v:02X}` format
    # (2 * length == 2), so existing 1-byte selects are unaffected.
    assert "    0x00: " in body
    assert "    0x02: " in body


def test_two_byte_value_overflow_falls_through_to_number():
    # A value that does not fit 2 bytes must NOT be emitted as a select
    # (select.py's _validate_options would reject it); it degrades to a number.
    ev = _event(
        byte_length=2,
        values=[_enum_value(0x10000, "TooBig")],
    )
    platform, _lines = gc.emit_entity(ev, "full")
    assert platform == "number"


def test_ecnsys_90_byte_block_expands_to_ten_system_slots():
    # The Vitotronic SYSTEM history: one 90-byte ecnsysEventType~Error block
    # -> ten 9-byte slots at base+i*9, slot 1 = "Letzter Fehler" (enabled),
    # later slots disabled. Mirrors vcontrold getError0..9 and the
    # hardware-confirmed VScotHO1_72 layout at 0x7507.
    ev = _event(byte_length=90, values=[])
    ev.address = 0x7507
    ev.tech = ev.token = ev.name = "ecnsysEventType~Error"
    entries = gc._error_history_entries(ev)
    assert len(entries) == 10
    assert entries[0]["address"] == 0x7507 and entries[0]["name"] == "Letzter Fehler"
    assert entries[0]["disabled"] is False and entries[0]["system"] is True
    assert entries[1]["address"] == 0x7510 and entries[1]["disabled"] is True
    assert entries[9]["address"] == 0x7507 + 81


def test_fehlerhis_fa_slots_are_gfa_without_codes():
    # FehlerHisFA* is the Feuerungsautomat archive: different subsystem,
    # different code space -> GFA naming, disabled, and system=False so
    # _error_history_lines never attaches the Vitotronic codes map.
    ev = _event(byte_length=9, values=[])
    ev.address = 0x7590
    ev.tech = ev.token = ev.name = "FehlerHisFA01"
    entries = gc._error_history_entries(ev)
    assert len(entries) == 1
    e = entries[0]
    assert e["name"] == "GFA Fehler 01" and e["disabled"] is True and e["system"] is False
    lines = "\n".join(gc._error_history_lines(e, "gfa_fehler_01", {0x38: "Kesselsensor"}, "vd300"))
    assert "codes:" not in lines
    assert "disabled_by_default: true" in lines


def test_single_system_slot_stays_letzter_fehler():
    # The mini-fixture / address-fallback path: a single sub-90-byte system
    # event emits one enabled "Letzter Fehler" slot at its own address.
    ev = _event(byte_length=9, values=[])
    ev.address = 0x7507
    ev.tech = ev.token = ev.name = "Error_Time"
    entries = gc._error_history_entries(ev)
    assert len(entries) == 1
    assert entries[0]["name"] == "Letzter Fehler" and entries[0]["system"] is True
