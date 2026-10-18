"""Regression tests for the gen_catalog / vitocatalog refactoring and audit fixes.

Covers:
  1. 4-tuple seen_addr deduplication on deferred error-history slots (P300 & GWG).
  2. Bare-hex --sw parsing in main() when printing the "floor" match warning.
  3. Default error_code_set="auto" in generate() matching the CLI default.
  4. SimpleNamespace duck-typing on Div2/Div10 converters without parameter_type.
  5. Menu-tree cache invalidation when _groups / _glinks are mutated in-place.
  6. EntitySpec / CommentSpec IR parity with emit_entity().

Run::

    python -m pytest tests/unit/test_gen_catalog_refactor_regressions.py -q
"""

from __future__ import annotations

import os
import sys
from types import SimpleNamespace

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402

_FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


# --- 1. Error-history 4-tuple deduplication in generate() -------------------


def _make_minimal_catalog(events: list[gc.Event], ident_rows: list[dict] | None = None, textmap: dict | None = None):
    """Construct a minimal in-memory Catalog for targeted generate() tests."""
    raw_events = {}
    links = {"dp1": [e.id for e in events]}
    cat = gc.Catalog(
        types={"TestDev": "dp1"},
        dp_name={"dp1": "Test Device"},
        links=links,
        raw_events=raw_events,
        vlinks={},
        raw_values={},
        access={},
        ident_rows=ident_rows
        or [
            {"token": "TestDev", "ident": 0x20CB, "ext": 0x0148, "extt": 0x0148, "f0": None, "f0t": None, "error_type": "1"}
        ],
        textmap=textmap or {},
    )
    cat._events = {e.id: e for e in events}
    return cat


def test_error_history_deduplicates_against_4_tuple_seen_addr():
    # Previously, _field_key(ev) inserted a 4-tuple (addr, byte_pos, bit_pos, access)
    # into seen_addr, while the deferred_history second pass checked a 3-tuple
    # (addr, 0, 0) -- which never matched a 4-tuple in seen_addr.
    ordinary_sensor = gc.Event(
        id="1",
        name="Outside Temp",
        address=0x0800,
        conversion="Div10",
        access_type=1,
        block_length=2,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="Outside_Temp",
        token="Outside_Temp~0x0800",
    )
    # Duplicate FehlerHisFA01 events pointing at the exact same address 0x7590
    fa_a = gc.Event(
        id="2",
        name="FehlerHisFA01",
        address=0x7590,
        conversion="",
        access_type=1,
        block_length=9,
        byte_length=9,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="FehlerHisFA01",
        token="FehlerHisFA01~0x7590",
    )
    fa_b = gc.Event(
        id="3",
        name="FehlerHisFA01_dup",
        address=0x7590,
        conversion="",
        access_type=1,
        block_length=9,
        byte_length=9,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="FehlerHisFA01_dup",
        token="FehlerHisFA01~0x7590",
    )
    # And an ordinary datapoint already claiming 0x7591 before deferred_history runs
    ordinary_at_fa02 = gc.Event(
        id="4",
        name="Already Claimed",
        address=0x7591,
        conversion="NoConversion",
        access_type=1,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="Already_Claimed",
        token="Already_Claimed~0x7591",
    )
    fa_at_claimed = gc.Event(
        id="5",
        name="FehlerHisFA02",
        address=0x7591,
        conversion="",
        access_type=1,
        block_length=9,
        byte_length=9,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="FehlerHisFA02",
        token="FehlerHisFA02~0x7591",
    )

    cat = _make_minimal_catalog([ordinary_sensor, fa_a, fa_b, ordinary_at_fa02, fa_at_claimed])
    out = gc.generate(cat, "TestDev", "full", None, None, error_codes=False)

    # 0x7590 appears once (fa_b is deduplicated)
    assert out.count("address: 0x7590") == 1
    # 0x7591 is claimed by the ordinary sensor in pass 1, so fa_at_claimed is suppressed in pass 2
    assert out.count("address: 0x7591") == 1
    assert "GFA Fehler 02" not in out


def test_gwg_error_history_slots_at_same_address_in_different_modes_do_not_collide():
    # Under GWG, address space is per access mode: two FehlerHis slots at 0x50
    # in different FCRead modes (EEPROM_READ vs Physical_READ) must both survive.
    ordinary_gwg = gc.Event(
        id="1",
        name="Kesseltemp",
        address=0x70,
        conversion="Div2",
        access_type=1,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        fc_read="Physical_READ",
        tech="Kesseltemp",
        token="Kesseltemp~0x0070",
    )
    fa_eeprom = gc.Event(
        id="2",
        name="FehlerHisFA01",
        address=0x50,
        conversion="",
        access_type=1,
        block_length=9,
        byte_length=9,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        fc_read="EEPROM_READ",
        tech="FehlerHisFA01",
        token="FehlerHisFA01~0x0050",
    )
    fa_phys = gc.Event(
        id="3",
        name="FehlerHisFA02",
        address=0x50,
        conversion="",
        access_type=1,
        block_length=9,
        byte_length=9,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        fc_read="Physical_READ",
        tech="FehlerHisFA02",
        token="FehlerHisFA02~0x0050",
    )
    cat = _make_minimal_catalog([ordinary_gwg, fa_eeprom, fa_phys])
    out = gc.generate(cat, "TestDev", "full", None, None, protocol=gc.PROTO_GWG, error_codes=False)
    assert out.count("address: 0x0050") == 2
    assert "access: eeprom" in out
    assert "access: physical" in out


# --- 2. Bare-hex --sw parsing on "floor" warning in main() ------------------


@pytest.mark.parametrize(
    ("sw_arg", "expected_hex"),
    [
        ("5A", "0x5A"),  # previously raised ValueError: int("5A", 0)
        ("0x5A", "0x5A"),  # 0x-prefixed hex
        ("7A", "0x7A"),  # issue #21 real GWG SW byte (bare hex)
        ("51", "0x51"),  # previously printed 0x33 because int("51", 0) parsed as base-10
    ],
)
def test_main_identify_floor_warning_handles_bare_and_prefixed_hex(monkeypatch, capsys, tmp_path, sw_arg, expected_hex):
    # Fixture VTestHO1_99 has Identification 0x20CB and range 0x0100..0x0140,
    # so any SW > 0x40 triggers a "floor" resolution.
    ev = gc.Event(
        id="1",
        name="Outside Temp",
        address=0x0800,
        conversion="Div10",
        access_type=1,
        block_length=2,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="Outside_Temp",
        token="Outside_Temp~0x0800",
    )
    ident_rows = [
        {
            "token": "TestDev",
            "Identification": "20CB",
            "IdentificationExtension": "0110",
            "IdentificationExtensionTill": "0120",
            "ident": 0x20CB,
            "ext": 0x0110,
            "extt": 0x0120,
            "f0": None,
            "f0t": None,
            "error_type": "1",
        }
    ]
    cat = _make_minimal_catalog([ev], ident_rows=ident_rows)
    monkeypatch.setattr(gc, "load_catalog", lambda *a, **kw: cat)

    out_file = tmp_path / "out.yaml"
    rc = gc.main(
        [
            "--data",
            _FIXTURE_DIR,
            "--identify",
            "0x20CB",
            "--hw",
            "01",
            "--sw",
            sw_arg,
            "--out",
            str(out_file),
        ]
    )
    assert rc == 0
    err = capsys.readouterr().err
    assert f"WARNING: SW {expected_hex} is beyond every range" in err


# --- 3. Default error_code_set="auto" in generate() -------------------------


def test_generate_defaults_to_auto_error_code_set():
    # When a catalog carries per-device Vitosoft fault texts, calling generate()
    # without specifying error_code_set must use "auto" (selecting the device's
    # Vitosoft texts), matching the CLI default rather than hardcoding "vd300".
    ev_sensor = gc.Event(
        id="1",
        name="Outside Temp",
        address=0x0800,
        conversion="Div10",
        access_type=1,
        block_length=2,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="Outside_Temp",
        token="Outside_Temp~0x0800",
    )
    ev_err = gc.Event(
        id="2",
        name="ecnsysEventType~Error",
        address=0x7507,
        conversion="",
        access_type=1,
        block_length=9,
        byte_length=9,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="ecnsysEventType~Error",
        token="ecnsysEventType~Error",
    )
    texts = {
        "viessmann.errorcode.TestDev.10": "Custom Vitosoft Device Fault 10",
    }
    cat = _make_minimal_catalog([ev_sensor, ev_err], textmap=texts)
    out = gc.generate(cat, "TestDev", "full", None, None)
    assert "Custom Vitosoft Device Fault 10" in out
    assert "viessmann.errorcode.TestDev.*" in out


# --- 4. SimpleNamespace duck-typing on Div2/Div10 without parameter_type ----


@pytest.mark.parametrize("conv", ["Div2", "Div10"])
@pytest.mark.parametrize("access_type", [1, 3])
def test_simplenamespace_without_parameter_type_works_on_div2_div10(conv, access_type):
    # Stand-in objects in test_gen_catalog_multibyte_select.py omit parameter_type
    # and unit_token. Calling emit_entity with Div2/Div10 must not raise AttributeError.
    ev = SimpleNamespace(
        id="1",
        name="Duck Temp",
        address=0x0800,
        conversion=conv,
        access_type=access_type,
        block_length=2,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        lower="0",
        upper="100",
        stepping="0.5",
        enum_type=False,
        unit="°C",
        tech="duck_temp",
        token="Duck~0x0800",
        fc_read="",
        fc_write="",
        conv_factor=None,
        conv_offset=None,
        block_factor=None,
        values=[],
    )
    platform, lines = gc.emit_entity(ev, "full")
    assert platform == ("number" if access_type == 3 else "sensor")
    assert any(f"converter: {conv.lower()}" in ln for ln in lines)


# --- 5. Menu cache invalidation on in-place _glinks mutation ----------------


def test_menu_cache_invalidates_when_glinks_gains_a_key():
    # The cache is keyed on (len(_groups), len(_glinks)): it notices a NEW key,
    # not an append to an existing key's list.
    cat = gc.load_catalog(_FIXTURE_DIR)
    cat._groups["9100"] = {"path": "VOther_01~10_Bedienung", "dp": "102", "order": 1}
    cat._glinks["1"] = [("9100", 1)]

    # Prime the cache
    first = [e.id for e in cat.events_for("VOther_01")]
    assert first == ["1"]

    # Add a group and a link under a new event key without recreating Catalog
    cat._groups["9101"] = {"path": "VOther_01~20_Warmwasser", "dp": "102", "order": 2}
    cat._glinks.setdefault("6", []).append(("9101", 1))

    second = [e.id for e in cat.events_for("VOther_01")]
    assert second == ["1", "6"]


# --- 6. EntitySpec / CommentSpec IR parity with emit_entity() ---------------


def test_build_entity_spec_and_render_match_emit_entity():
    cat = gc.load_catalog(_FIXTURE_DIR)
    for ev in cat.events_for("VTestHO1_99"):
        legacy = gc.emit_entity(ev, "full")
        spec = gc.build_entity_spec(ev, "full")
        if legacy is None:
            assert spec is None
        elif legacy[0] == "comment":
            assert isinstance(spec, gc.CommentSpec)
            assert spec.lines == legacy[1]
        else:
            assert isinstance(spec, gc.EntitySpec)
            assert spec.platform == legacy[0]
            assert gc.render_entity_lines(spec) == legacy[1]


# --- 7. Line order of a diagnostic entity matches the pre-refactor output ----


def test_diagnostic_entity_keeps_id_before_entity_category():
    # The pre-refactor generator injected entity_category after name, then
    # inserted id right after name, so id came first. The committed catalogs
    # carry that order; emitting it the other way round rewrites ~14k lines.
    ev = gc.Event(
        id="1",
        name="Sammelstoerung",
        address=0x0A10,
        conversion="NoConversion",
        access_type=1,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="WPR_Sammelstoerung",
        token="WPR_Sammelstoerung~0x0A10",
    )
    cat = _make_minimal_catalog([ev])
    lines = gc.generate(cat, "TestDev", "full", None, None, emit_device_id=False).splitlines()
    i = lines.index('    name: "Sammelstoerung"')
    assert lines[i + 1 : i + 4] == [
        "    id: WPR_Sammelstoerung",
        "    entity_category: diagnostic",
        "    address: 0x0A10",
    ]
