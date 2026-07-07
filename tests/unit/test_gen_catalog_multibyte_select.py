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


# --- Audit SS4.2: interior (offset) fields on the writable branches ----------
# The block-base alignment (use_block_extract) was once applied to addr_line
# for ALL branches while only the sensor branch emitted byte_offset, so an
# interior select/switch/number/enum read the block base with the field
# length -- the wrong bytes. select/switch now emit the two-address block
# form (state_address = block base + byte_offset; address = the field's own
# write register); number and the read-only enum revert to the interior
# address with the P300-may-NAK caveat.


def test_interior_writable_enum_emits_two_address_select():
    ev = _event(
        address=0x7660,
        block_length=2,
        byte_length=1,
        byte_position=1,
        values=[_enum_value(0x00, "Aus"), _enum_value(0x02, "Heizen")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "select"
    # write target = the field's own register; state read = aligned block.
    assert "address: 0x7661" in body
    assert "state_address: 0x7660" in body
    assert "length: 2" in body
    assert "byte_offset: 1" in body
    # 1-byte field -> no byte_length line needed (component default).
    assert "byte_length" not in body


def test_interior_boolean_pair_emits_two_address_switch():
    ev = _event(
        address=0x7660,
        block_length=4,
        byte_length=1,
        byte_position=2,
        values=[_enum_value(0x00, "AUS"), _enum_value(0x01, "EIN")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "switch"
    assert "address: 0x7662" in body
    assert "state_address: 0x7660" in body
    assert "length: 4" in body
    assert "byte_offset: 2" in body


def test_interior_multibyte_enum_field_emits_byte_length():
    ev = _event(
        address=0x7660,
        block_length=6,
        byte_length=2,
        byte_position=4,
        values=[_enum_value(0x0000, "Stufe 1"), _enum_value(0x0100, "Stufe 2")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "select"
    assert "address: 0x7664" in body
    assert "state_address: 0x7660" in body
    assert "length: 6" in body
    assert "byte_offset: 4" in body
    assert "byte_length: 2" in body
    # Option keys stay padded to the FIELD width, not the block read.
    assert "    0x0000: " in body


def test_interior_field_with_command_state_mapping_keeps_interior_form():
    # A COMMAND_STATE_ADDR entry claims state_address for the read/write
    # split; the block-extract form cannot also use it, so the entity falls
    # back to the interior address with the caveat.
    addr = next(iter(gc.COMMAND_STATE_ADDR))
    ev = _event(
        address=addr,
        block_length=2,
        byte_length=1,
        byte_position=1,
        values=[_enum_value(0x00, "Aus"), _enum_value(0x02, "Heizen")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "select"
    assert f"address: 0x{addr + 1:04X}" in body
    assert "byte_offset" not in body
    assert "P300 may NAK" in body
    assert f"state_address: 0x{gc.COMMAND_STATE_ADDR[addr]:04X}" in body


def test_interior_writable_number_emits_two_address_form():
    # The pump-speed shape (Neptun * Drehzahl): a writable 1-byte numeric at
    # byte 1 of a 2-byte block. Read = aligned block at the base via
    # state_address + byte_offset; write = the field's own register.
    ev = _event(
        address=0x7951,
        block_length=2,
        byte_length=1,
        byte_position=1,
        values=[],
        enum_type=False,
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "number"
    assert "address: 0x7952" in body  # write target: the field's own register
    assert "state_address: 0x7951" in body
    assert "length: 2" in body  # the block read
    assert "byte_offset: 1" in body
    assert "byte_length" not in body  # 1-byte field -> component default
    assert "P300 may NAK" not in body  # the read is aligned now


def test_interior_multibyte_number_field_emits_byte_length():
    ev = _event(
        address=0x1189,
        block_length=6,
        byte_length=2,
        byte_position=4,
        values=[],
        enum_type=False,
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "number"
    assert "address: 0x118D" in body
    assert "state_address: 0x1189" in body
    assert "length: 6" in body
    assert "byte_offset: 4" in body
    assert "byte_length: 2" in body


def test_interior_number_in_oversize_block_falls_back_to_interior():
    ev = _event(
        address=0x7660,
        block_length=gc.MAX_P300_READ_LENGTH + 5,
        byte_length=1,
        byte_position=40,
        values=[],
        enum_type=False,
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "number"
    assert "byte_offset" not in body
    assert "state_address" not in body
    assert "P300 may NAK" in body  # honest interior fallback


def test_interior_readonly_enum_emits_block_extraction():
    # Read-only: the block base IS the read address -- no write side, no
    # state_address. The AktuelleBetriebsart shape: byte 1 of the 22-byte
    # 0x2500 block.
    ev = _event(
        address=0x2500,
        access_type=1,  # read-only
        block_length=22,
        byte_length=1,
        byte_position=1,
        values=[_enum_value(0x00, "Aus"), _enum_value(0x02, "Heizen")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "text_sensor"
    assert "type: enum" in body
    assert "address: 0x2500" in body  # the block base, plain
    assert "state_address" not in body
    assert "length: 22" in body
    assert "byte_offset: 1" in body
    assert "byte_length" not in body  # enum_len 1 -> component default
    assert "P300 may NAK" not in body


def test_interior_readonly_enum_in_oversize_block_falls_back_to_interior():
    ev = _event(
        address=0x7660,
        access_type=1,
        block_length=gc.MAX_P300_READ_LENGTH + 5,
        byte_length=1,
        byte_position=40,
        values=[_enum_value(0x00, "Aus"), _enum_value(0x02, "Heizen")],
    )
    platform, lines = gc.emit_entity(ev, "full")
    body = "\n".join(lines)
    assert platform == "text_sensor"
    assert "byte_offset" not in body
    assert "P300 may NAK" in body
