"""Burner-control datapoints (Vitosoft FCRead GFA_READ) in scripts/gen_catalog.py.

They are read with `access: gfa` (the KW telegram 0x6B) and carry a
`gfa=<chips>` tag: the burner-control chips whose process-value menu lists them
on the device. Shaped after VScotHO1_72: P06 is in every chip's menu (through
two event variants that decode alike), P09 decodes as rpm on most chips and as
% on two, P31 is NEC-only, and one row sits in no chip menu at all.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_gen_catalog_gfa.py -q
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
from vitocatalog.loader import _gfa_menu_chips  # noqa: E402

_FK = "VGfaTest_01~60_Funktionskontrolle"
_GROUPS = {
    "ces": (f"{_FK}~35_FA_VSKO", "dp1"),
    "gfa": (f"{_FK}~35_FA_VSKO_GFA", "dp1"),
    "nec": (f"{_FK}~35_FA_VSKO_NEC", "dp1"),
    "scot": (f"{_FK}~35_FA_VSKO_Scot", "dp1"),
    # The export names some DOVER menus after another token, with this device's
    # name glued to the suffix; the DataPointTypeId is what ties it to dp1.
    "dov": ("VGfaTest_4~60_Funktionskontrolle~35_FA_VSKO_DOVERVGfaTest_01", "dp1"),
}

_RPM = {"conversion": "MultOffset", "conversion_factor": "30", "conversion_offset": "0", "unit": "ecnUnit.U pro min"}
_PCT = {"conversion": "MultOffset", "conversion_factor": "0.3922", "conversion_offset": "0", "unit": "ecnUnit.Prozent"}
_RAW = {"conversion": "NoConversion"}

# eid: (name, address, decode, FCRead, menus)
_EVENTS = {
    "1": ("VSKO_Scot_CES_P6", "4006", _RPM, "GFA_READ", ["ces", "gfa", "nec", "dov"]),
    "2": ("VSKO_Scot_P6", "4006", _RPM, "GFA_READ", ["scot"]),
    "3": ("VSKO_Scot_CES_P9", "4009", _RPM, "GFA_READ", ["ces", "nec", "scot"]),
    "4": ("VSKO_GFA_P9", "4009", _PCT, "GFA_READ", ["gfa", "dov"]),
    "5": ("VSKO_Scot_NEC_P31", "401F", _RAW, "GFA_READ", ["nec"]),
    "6": ("VSKO_Scot_CES_P78", "404E", _RAW, "GFA_READ", []),
    # The same address in the Vitotronic's own space: a different datapoint.
    "7": ("Schaltzeit_M3_Byte6", "4006", _RAW, "Virtual_READ", []),
}


def _catalog():
    raw_events, access, glinks = {}, {}, {}
    for eid, (name, addr, decode, fc_read, menus) in _EVENTS.items():
        token = f"{name}~0x{addr}"
        raw_events[eid] = {"Address": token, "Name": name, "Type": "1"}
        access[token] = {
            "address": addr,
            "block_length": "1",
            "byte_length": "1",
            "byte_position": "0",
            "fc_read": fc_read,
            "fc_write": "undefined",
            "parameter_type": "Byte",
            **decode,
        }
        glinks[eid] = [(gid, n) for n, gid in enumerate(menus)]
    groups = {gid: {"path": path, "dp": dp, "order": 0, "parent": "-1"} for gid, (path, dp) in _GROUPS.items()}
    return gc.Catalog(
        {"VGfaTest_01": "dp1"},
        {"dp1": "VGfaTest"},
        {"dp1": list(_EVENTS)},
        raw_events,
        {},
        {},
        access,
        [],
        {},
        groups=groups,
        glinks=glinks,
    )


def _generate(protocol="P300"):
    return gc.generate(
        _catalog(), "VGfaTest_01", "full", None, None, emit_device_id=False, emit_error_history=False, protocol=protocol
    )


def _block(out, entity_id):
    """The entity's lines, from its tag line (if any) to the blank line after it."""
    lines = out.splitlines()
    at = next(n for n, line in enumerate(lines) if line.strip() == f"id: {entity_id}")
    start = at
    while start > 0 and lines[start - 1].strip() and not lines[start].lstrip().startswith("# vito:"):
        start -= 1
    end = at
    while end < len(lines) and lines[end].strip():
        end += 1
    return "\n".join(lines[start:end])


@pytest.mark.parametrize(
    "path,chips",
    [
        (f"{_FK}~35_FA_VSKO", ("23",)),  # "Prozesswerte FA CES"
        (f"{_FK}~35_FA_VSKO_GFA", ("20",)),
        (f"{_FK}~35_FA_VSKO_Scot", ("21",)),
        (f"{_FK}~35_FA_VSKO_DOVER", ("22",)),
        ("VGfaTest_4~60_Funktionskontrolle~35_FA_VSKO_DOVERVScotHO1_72", ("22",)),
        (f"{_FK}~35_FA_VSKO_NEC", ("24",)),
        (f"{_FK}~35_FA_VSKO_ViCC_Hauptcontroller", ("27",)),
        (f"{_FK}~35_FA_VSKO_OEFA", ("41", "42")),
        (f"{_FK}~35_FA_VSKO_Unknown", ()),
        (f"{_FK}~10_Codierstecker", ()),
    ],
)
def test_menu_suffix_names_the_chip(path, chips):
    assert _gfa_menu_chips(path) == chips


def test_gfa_is_reachable_on_kw_and_p300_only():
    ev = _catalog()._events["1"]
    for protocol in (gc.PROTO_P300, gc.PROTO_KW):
        assert gc._is_gfa_read(ev, protocol)
        assert gc._is_reachable(ev, protocol)
        assert gc._access_for(ev, protocol) == "gfa"
    assert not gc._is_gfa_read(ev, gc.PROTO_GWG)
    assert not gc._is_reachable(ev, gc.PROTO_GWG)
    assert gc._access_for(ev, gc.PROTO_GWG) == ""
    # No GFA write exists: a GFA row never becomes writable.
    assert not gc._is_writable(ev, gc.PROTO_P300)


def test_chips_are_the_union_over_rows_that_decode_alike():
    cat = _catalog()

    def tags(eid):
        return cat.tags_for(cat._events[eid], "VGfaTest_01")

    # P06: four chips through one event, the fifth through its Scot twin.
    assert tags("1") == tags("2") == ["gfa=20,21,22,23,24"]
    # P09 decodes differently on the GFA and DOVER chips: separate chip sets.
    assert tags("3") == ["gfa=21,23,24"]
    assert tags("4") == ["gfa=20,22"]
    assert tags("5") == ["gfa=24"]
    # In no chip's menu: no tag, the datapoint applies whatever the chip.
    assert tags("6") == []
    # Not a GFA datapoint at all.
    assert tags("7") == []


@pytest.mark.parametrize("protocol", ["P300", "KW"])
def test_gfa_rows_are_emitted_with_access_gfa(protocol):
    out = _generate(protocol)
    p6 = _block(out, "VSKO_Scot_CES_P6")
    assert "# vito: gfa=20,21,22,23,24" in p6
    assert "access: gfa  # Vitosoft GFA_READ" in p6
    assert "multiply: 30" in p6
    # The Scot twin folded into it.
    assert "id: VSKO_Scot_P6" not in out
    # P09 is two entities, one per decode, each with its own chips.
    assert "# vito: gfa=21,23,24" in _block(out, "VSKO_Scot_CES_P9")
    pct = _block(out, "VSKO_GFA_P9")
    assert "# vito: gfa=20,22" in pct and "multiply: 0.3922" in pct and "access: gfa" in pct
    assert "access: gfa" in _block(out, "VSKO_Scot_CES_P78")
    # The Vitotronic's own 0x4006 stays a plain read beside the GFA one.
    own = _block(out, "Schaltzeit_M3_Byte6")
    assert "address: 0x4006" in own and "access:" not in own


def test_header_counts_the_gfa_entities_and_their_chips():
    out = _generate()
    assert "# Burner control: 5 entities read in the burner control's own" in out
    assert "(here 20, 21, 22, 23, 24)" in out
    assert "FCRead filter ON: 0 datapoint" in out


def test_gfa_entities_are_read_only():
    out = _generate()
    for entity_id in ("VSKO_Scot_CES_P6", "VSKO_GFA_P9", "VSKO_Scot_NEC_P31"):
        block = _block(out, entity_id)
        assert "- platform: vitohome" in block
    assert "number:" not in out and "select:" not in out and "switch:" not in out
