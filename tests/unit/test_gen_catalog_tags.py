"""Metadata tags (`# vito:`) and product names in scripts/gen_catalog.py.

Vitosoft's own grouping rides along with each entity as one comment line the
catalog editor filters on: the circuit a menu belongs to (LogicalGroupMapping,
inherited by its sub-menus), the device's own system menus (overview screen,
trend recorder, service access level) and the quick-check option. The type's
ShortName / ProductName go to the catalog header and the finder's table.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_gen_catalog_tags.py -q
"""

import os
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402

# Menus of device dp1 ("VTagTest_01") and one of another device, dp2. Circuit
# mappings sit one level below a section, as in the export.
_GROUPS = {
    "g10": ("VTagTest_01~10_Bedienung_A1", "dp1", "-1"),
    "g11": ("VTagTest_01~10_Bedienung_A1~10_Betriebsdaten", "dp1", "g10"),  # 1 -> hk1
    "g12": ("VTagTest_01~10_Bedienung_A1~10_Betriebsdaten~05_Detail", "dp1", "g11"),  # inherits hk1
    "g20": ("VTagTest_01~35_Information~40_Warmwasser", "dp1", "-1"),  # 20 -> dhw
    "g21": ("VTagTest_01~35_Information~40_Warmwasser~10_Aus", "dp1", "g20"),  # 0: explicitly none
    "g30": ("VTagTest_01~60_Funktionskontrolle~10_Codierstecker", "dp1", "-1"),  # 25 -> cascade
    "s1": ("ecnsysEventTypeGroup~VTagTest_01~Overview", "dp1", "-1"),
    "s2": ("ecnsysEventTypeGroup~VTagTest_01~Trending", "dp1", "-1"),
    "s3": ("ecnsysEventTypeGroup~VTagTest_01~CodeAccessLevelTD", "dp1", "-1"),
    "s4": ("ecnsysEventTypeGroup~VTagTest_01~Lasterror", "dp1", "-1"),  # no tag of its own
    "s41": ("VTagTest_01~Lasterror~10_Heizkreis", "dp1", "s4"),  # 2 -> hk2, under a system menu
    "o1": ("VOther_01~10_Bedienung~10_Betriebsdaten", "dp2", "-1"),  # 3 -> hk3, other device
}
_MAPPING = {"g11": 1, "g20": 20, "g21": 0, "g30": 25, "s41": 2, "o1": 3}

# eid: (token, menus)
_EVENTS = {
    "1": ("RaumtemperaturA1M1~0x0896", ["g12", "s1", "s2"]),
    "2": ("WarmwasserTemp~0x0804", ["g20", "g11"]),
    "3": ("WarmwasserAus~0x0805", ["g21"]),
    "4": ("Codierstecker~0x1060", ["g30", "s3"]),
    "5": ("LetzterFehlerHK2~0x7700", ["s41"]),
    "6": ("Ungetaggt~0x0900", []),
    "7": ("Aussentemperatur~0x0800", ["o1"]),
}


def _catalog(products=True):
    raw_events, access, glinks = {}, {}, {}
    for eid, (token, menus) in _EVENTS.items():
        raw_events[eid] = {"Address": token, "Name": token.split("~")[0], "Type": "1"}
        access[token] = {
            "address": token.split("~")[1],
            "block_length": "2",
            "byte_length": "2",
            "byte_position": "0",
            "conversion": "Div10",
            "fc_read": "Virtual_READ",
            "fc_write": "undefined",
            "parameter_type": "Int",
        }
        glinks[eid] = [(gid, n) for n, gid in enumerate(menus)]
    groups = {gid: {"path": path, "dp": dp, "order": 0, "parent": parent} for gid, (path, dp, parent) in _GROUPS.items()}
    ext = {
        ("ecnEventTypeGroup", "LogicalGroupMapping"): _MAPPING,
        ("ecnEventType", "Option"): {"2": "NoLogging;Schnellcheck"},
    }
    if products:
        ext[("ecnDatapointType", "ShortName")] = {"dp1": "VT 200  (HO1C)"}
        ext[("ecnDatapointType", "ProductName")] = {"dp1": "Vitodens 3xx-W mit\nVitotronic 200 (Typ HO1C)"}
    return gc.Catalog(
        {"VTagTest_01": "dp1", "VOther_01": "dp2"},
        {"dp1": "VTagTest", "dp2": "VOther"},
        {"dp1": [e for e in _EVENTS if e != "7"], "dp2": ["7"]},
        raw_events,
        {},
        {},
        access,
        [],
        {},
        groups=groups,
        glinks=glinks,
        ext=ext,
    )


def _tags(cat, eid, device="VTagTest_01"):
    return cat.tags_for(cat._events[eid], device)


def test_circuit_comes_from_the_menu_or_its_nearest_ancestor():
    cat = _catalog()
    # g12 sets nothing and inherits hk1 from g11; the system menus add flags.
    assert _tags(cat, "1") == ["circuit=hk1", "overview", "trend"]
    # Two circuits are listed in Vitosoft's order (heating circuits first).
    assert _tags(cat, "2") == ["circuit=hk1,dhw", "quickcheck"]
    # An explicit 0 under a dhw menu means no circuit, not its parent's.
    assert _tags(cat, "3") == []
    # 25 (not in Vitosoft's option list) sits on the cascade menus only.
    assert _tags(cat, "4") == ["circuit=cascade", "level=td"]
    # A sub-menu of a system menu keeps its own circuit; a system menu without
    # a tag of its own (Lasterror) adds nothing.
    assert _tags(cat, "5") == ["circuit=hk2"]
    assert _tags(cat, "6") == []


def test_tags_are_per_device():
    cat = _catalog()
    # Event 7 is in another device's hk3 menu: not a tag of this one.
    assert _tags(cat, "7") == []
    assert _tags(cat, "7", "VOther_01") == ["circuit=hk3"]


def test_the_tag_line_sits_right_above_its_entity():
    out = gc.generate(_catalog(), "VTagTest_01", "full", None, None, emit_device_id=False, emit_error_history=False)
    lines = out.splitlines()
    tagged = [n for n, line in enumerate(lines) if line.startswith("  # vito: ")]
    assert len(tagged) == 4  # events 1, 2, 4 and 5; 3 and 6 carry none
    for n in tagged:
        assert lines[n + 1] == "  - platform: vitohome"
    block = out[out.index("# vito: circuit=hk1 overview trend") :].split("\n\n", 1)[0]
    assert "address: 0x0896" in block
    assert "# Metadata: 4 entities carry a `# vito:` line" in out


def test_product_names_go_to_the_header_and_the_manifest():
    cat = _catalog()
    # Whitespace runs (line breaks in the export) collapse.
    assert cat.product_for("VTagTest_01") == ("VT 200 (HO1C)", "Vitodens 3xx-W mit Vitotronic 200 (Typ HO1C)")
    assert cat.product_for("VOther_01") == ("", "")
    out = gc.generate(cat, "VTagTest_01", "full", None, None, emit_device_id=False, emit_error_history=False)
    header = " ".join(line.lstrip("#").strip() for line in out.splitlines()[:6])  # wrapped at 70 columns
    assert "product (Vitosoft): Vitodens 3xx-W mit Vitotronic 200 (Typ HO1C) [VT 200 (HO1C)]" in header
    assert "# product" not in gc.generate(_catalog(products=False), "VTagTest_01", "full", None, None)

    manifest = [
        {
            "token": "VTagTest_01",
            "file": "vtagtest_01.yaml",
            "status": "ok",
            "short_name": "VT 200 (HO1C)",
            "product_name": "Vitodens",
        },
        {"token": "VOther_01", "file": "vother_01.yaml", "status": "ok", "short_name": "", "product_name": ""},
    ]
    tokens = gc.build_resolve_index(cat, manifest)["tokens"]
    assert tokens["VTagTest_01"] == {
        "file": "vtagtest_01.yaml",
        "status": "ok",
        "detail": "ok",
        "short": "VT 200 (HO1C)",
        "product": "Vitodens",
    }
    assert "product" not in tokens["VOther_01"]
