"""Writes narrower than their block become whole-block read-modify-writes.

Hardware, VScotHO1_72 (P300), 2026-10-14: Neptun_ZP~0x7953 is a 2-byte block
whose byte 1 read 00 before any write. After a 1-byte write the device held
01 D6, then 00 D5 -- byte 1 became the checksum byte of our own write telegram.
The device writes its whole datapoint and fills it from whatever follows the
payload. The ACK and a 1-byte read-back both look fine, so the generator, which
knows the block length, emits such a field in block mode (address = the block
base, length = the block, byte_offset = the field): the component reads the
block, replaces the field and writes the whole block back. A block that also
holds a READ-ONLY datapoint (live data) stays read-only.

    python -m pytest tests/unit/test_gen_catalog_narrow_write.py -q
"""

import os
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog.constants import MAX_RMW_BLOCK, PROTO_P300  # noqa: E402
from vitocatalog.emitter import emit_entity  # noqa: E402
from vitocatalog.models import Event, EventValue  # noqa: E402
from vitocatalog.transforms import _access_for, _read_only_blocks  # noqa: E402


def _ev(values=None, **over):
    """Vitotwin_1013_Leistungsgrenze_Haupterzeuger-shaped: a writable 1-byte
    field at byte 0 of a 2-byte block."""
    kw = {
        "id": "t",
        "name": "Vitotwin 1013 Leistungsgrenze Haupterzeuger",
        "tech": "Vitotwin_1013_Leistungsgrenze_Haupterzeuger",
        "address": 0x1013,
        "conversion": "NoConversion",
        "access_type": 3,
        "block_length": 2,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "0",
        "upper": "100",
        "stepping": "1",
        "fc_read": "Virtual_READ",
        "fc_write": "Virtual_WRITE",
    }
    kw.update(over)
    ev = Event(**kw)
    if values:
        ev.values = [EventValue(enum_address_value=v, description=label) for v, label in values]
    return ev


def _key(lines, key):
    for ln in lines:
        s = ln.strip()
        if s.startswith(key + ":"):
            return s.split(":", 1)[1].strip()
    return None


def _geometry(lines):
    return tuple(_key(lines, k) for k in ("address", "length", "byte_offset", "byte_length"))


def test_narrow_numeric_write_is_a_block_write():
    platform, lines = emit_entity(_ev(), "full", PROTO_P300)
    assert platform == "number"
    # byte_offset 0 is emitted: without it the 1-byte field would be a narrow write.
    assert _geometry(lines) == ("0x1013", "2", "0", None)
    assert "read-modify-write of the whole 2-byte block" in "\n".join(lines)
    assert _key(lines, "state_address") is None


def test_narrow_two_byte_write_in_a_three_byte_block_is_a_block_write():
    # Vitotwin_2000_Komfortsollwert: 2 bytes of a 3-byte block.
    ev = _ev(address=0x2000, block_length=3, byte_length=2, conversion="Div10", lower="10", upper="30", stepping="0.5")
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "number"
    assert _geometry(lines) == ("0x2000", "3", "0", "2")


def test_narrow_enum_write_is_a_block_write():
    # Vitotwin_1030_Brenner_sperren: a select on byte 0 of a 2-byte block.
    ev = _ev(values=[(0, "frei"), (1, "gesperrt"), (2, "extern")], lower="", upper="", stepping="")
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "select"
    assert _geometry(lines) == ("0x1013", "2", "0", None)


def test_block_with_a_read_only_datapoint_stays_read_only():
    # A read-only sibling means the block carries live data: writing it back
    # from the read would restore a value the device may have changed since.
    ro = frozenset({(0x1013, _access_for(_ev(), PROTO_P300))})
    platform, lines = emit_entity(_ev(), "full", PROTO_P300, ro_blocks=ro)
    assert platform == "sensor"
    text = "\n".join(lines)
    assert "read-only (live) data" in text
    assert "demoted to read-only" in text
    assert "min_value" not in text


def test_read_only_blocks_over_the_whole_device():
    # The read-only datapoint at byte 1 marks the block; the writable one does not.
    writable = _ev(id="w")
    ro = _ev(id="r", name="Vitotwin 1013 Ist", tech="Vitotwin_1013_Ist", byte_position=1, access_type=1, fc_write="")
    access = _access_for(writable, PROTO_P300)
    assert _read_only_blocks([writable], PROTO_P300) == frozenset()
    assert _read_only_blocks([writable, ro], PROTO_P300) == frozenset({(0x1013, access)})


def test_block_larger_than_the_rmw_cap_stays_read_only():
    ev = _ev(block_length=MAX_RMW_BLOCK + 1)
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "sensor"
    assert f"exceeds the {MAX_RMW_BLOCK}-byte read-modify-write" in "\n".join(lines)


def test_no_partial_writes_keeps_narrow_fields_read_only():
    platform, lines = emit_entity(_ev(), "full", PROTO_P300, partial_writes=False)
    assert platform == "sensor"
    assert "--no-partial-writes" in "\n".join(lines)
    # A full-width write is not a partial write and is unaffected.
    platform, _ = emit_entity(_ev(block_length=1), "full", PROTO_P300, partial_writes=False)
    assert platform == "number"


def test_full_width_write_is_unchanged():
    # BedienRTSolltemperaturA1M1 (0x2306): a true 1-byte datapoint stays writable.
    ev = _ev(address=0x2306, block_length=1, byte_length=1, lower="3", upper="37")
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "number"
    assert "byte(s) of a" not in "\n".join(lines)


def test_whole_block_write_is_unchanged():
    # A 2-byte field that IS the whole 2-byte block writes the whole datapoint.
    ev = _ev(block_length=2, byte_length=2, lower="0", upper="1000")
    platform, _ = emit_entity(ev, "full", PROTO_P300)
    assert platform == "number"
