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
    ev = _event(
        byte_length=2,
        values=[_enum_value(0x0000, "Off"), _enum_value(0x0100, "On")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "select"
    assert "  length: 2" in body
    # Option keys are zero-padded to the field width (2 bytes -> 4 hex digits).
    assert "    0x0000: " in body
    assert "    0x0100: " in body


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
