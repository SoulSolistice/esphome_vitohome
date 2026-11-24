"""Hardware findings (scripts/vitocatalog/confirmed.py, confirmed.toml).

A finding corrects or completes the Vitosoft export for the device tokens it
names and for no other: the shared export row stays untouched, so a sibling
revision without a confirmation keeps Vitosoft's definition.

    python -m pytest tests/unit/test_confirmed_findings.py -q
"""

import csv
import glob
import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog import confirmed  # noqa: E402
from vitocatalog.confirmed import FindingError  # noqa: E402
from vitocatalog.constants import PROTO_GWG  # noqa: E402
from vitocatalog.loader import Catalog  # noqa: E402
from vitocatalog.models import Event  # noqa: E402
from vitocatalog.renderer import generate  # noqa: E402

_BASE = {
    "checked": "read",
    "what": "seen on the unit",
    "unit": "0x2053 HW=0x01 SW=0x1E",
    "source": "SoulSolistice/esphome_vitohome#35",
}


_NAME = {"de": "Wert neu", "en": "New value"}
_DEFINE = {
    "address": 0x40,
    "conversion": "NoConversion",
    "access_type": 1,
    "block_length": 1,
    "byte_length": 1,
    "byte_position": 0,
    "fc_read": "EEPROM_READ",
}


def _finding(**over):
    entry = {**_BASE, **over}
    return confirmed.parse({"finding": [entry]})[0]


def _ev(eid, tech, address, **over):
    kw = {
        "id": eid,
        "name": tech,
        "tech": tech,
        "token": f"{tech}~0x{address:04X}",
        "address": address,
        "conversion": "NoConversion",
        "access_type": 1,
        "block_length": 1,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "0",
        "upper": "255",
        "stepping": "1",
        "fc_read": "EEPROM_READ",
        "fc_write": "",
    }
    kw.update(over)
    return Event(**kw)


def _catalog():
    """GWG_A and GWG_B share the pressure row; only GWG_B has the speed row."""
    pressure = _ev("1", "GWG_Wasserdruck", 0x88, block_length=2, byte_length=2, fc_read="XRAM_READ", unit="bar")
    limit = _ev("2", "GWG_K45Max", 0x0C, access_type=3, fc_write="EEPROM_WRITE", lower="1", upper="100")
    speed = _ev("3", "GWG_K46Red", 0x0D, access_type=3, fc_write="EEPROM_WRITE", lower="1", upper="100")
    events = [pressure, limit, speed]
    cat = Catalog(
        types={"GWG_A": "dpA", "GWG_B": "dpB"},
        dp_name={"dpA": "GWG A", "dpB": "GWG B"},
        links={"dpA": ["1", "2"], "dpB": ["1", "2", "3"]},
        raw_events={},
        vlinks={},
        raw_values={},
        access={},
        ident_rows=[],
        textmap={},
        groups={
            "gA": {"path": "GWG_A~20_Codierungen~30_Sonstiges", "dp": "dpA", "order": 0, "parent": ""},
        },
        glinks={"2": [("gA", 7)]},
    )
    cat._events = {e.id: e for e in events}
    return cat


def _by_tech(events):
    return {e.tech: e for e in events}


# --- parsing -----------------------------------------------------------------


def test_the_shipped_file_parses():
    assert confirmed.load()


@pytest.mark.parametrize(
    ("over", "message"),
    [
        ({"action": "fix"}, "action must be"),
        ({"checked": "seen"}, "checked must be"),
        ({"devices": []}, "devices must be"),
        ({"devices": ["GWG_A", "GWG_A"]}, "twice"),
        ({"set": {"name": "x"}}, "not settable"),
        ({"set": {"access_type": True}}, "must be int"),
        ({"set": {"big_endian": 1}}, "must be bool"),
        ({"action": "drop", "set": {"access_type": 1}}, "takes no set"),
        ({"action": "change", "set": {}}, "needs set"),
        ({"action": "change", "set": {"access_type": 1}, "group_of": "X"}, "group_of applies"),
        ({"action": "define", "from": "GWG_B", "name": _NAME, "set": _DEFINE}, "from applies"),
        ({"action": "confirm", "set": {"access_type": 1}}, "takes no set"),
        ({"action": "define", "set": _DEFINE}, "needs name"),
        ({"action": "define", "name": {"de": "x"}, "set": _DEFINE}, "needs name"),
        ({"action": "define", "name": _NAME, "set": {"address": 0x10}}, "needs set.conversion"),
        ({"name": _NAME}, "name applies to define only"),
        ({"source": ""}, "non-empty string"),
        ({"evidence": "x"}, "unknown key"),
    ],
)
def test_malformed_findings_are_refused(over, message):
    entry = {**_BASE, "tech": "GWG_K46Red", "action": "add", "devices": ["GWG_A"], **over}
    with pytest.raises(FindingError, match=message):
        confirmed.parse({"finding": [entry]})


def test_a_missing_key_is_refused():
    entry = {**_BASE, "tech": "GWG_K46Red", "action": "add"}
    with pytest.raises(FindingError, match="missing devices"):
        confirmed.parse({"finding": [entry]})


def test_two_findings_for_one_datapoint_on_one_device_are_refused():
    a = {**_BASE, "tech": "GWG_K45Max", "action": "change", "devices": ["GWG_A"], "set": {"access_type": 1}}
    b = {**_BASE, "tech": "GWG_K45Max", "action": "drop", "devices": ["GWG_B", "GWG_A"]}
    with pytest.raises(FindingError, match="both name GWG_K45Max on GWG_A"):
        confirmed.parse({"finding": [a, b]})


# --- applying ----------------------------------------------------------------


def test_change_applies_to_the_named_device_only():
    cat = _catalog()
    cat.use_findings([_finding(tech="GWG_Wasserdruck", action="change", devices=["GWG_B"], set={"big_endian": True})])
    on_b = _by_tech(cat.events_for("GWG_B"))["GWG_Wasserdruck"]
    on_a = _by_tech(cat.events_for("GWG_A"))["GWG_Wasserdruck"]
    assert on_b.big_endian
    assert on_b.confirmed and "SoulSolistice/esphome_vitohome#35" in on_b.confirmed[0]
    assert not on_a.big_endian
    assert not on_a.confirmed
    assert not cat._events["1"].big_endian  # the shared row is not modified


def test_add_links_an_existing_row_and_joins_the_named_group():
    cat = _catalog()
    cat.use_findings([_finding(tech="GWG_K46Red", action="add", devices=["GWG_A"], group_of="GWG_K45Max")])
    added = _by_tech(cat.events_for("GWG_A"))["GWG_K46Red"]
    assert added.address == 0x0D
    assert added.confirmed
    assert cat.group_for(added, "GWG_A")[1] == cat.group_for(_by_tech(cat.events_for("GWG_A"))["GWG_K45Max"], "GWG_A")[1]
    assert cat.group_for(added, "GWG_A")[1] != "(ohne Gruppenzuordnung)"


def test_add_takes_set_overrides():
    cat = _catalog()
    cat.use_findings([_finding(tech="GWG_K46Red", action="add", devices=["GWG_A"], set={"access_type": 1})])
    assert _by_tech(cat.events_for("GWG_A"))["GWG_K46Red"].access_type == 1
    assert cat._events["3"].access_type == 3


def test_a_list_of_tech_ids_is_one_finding_each():
    entry = {**_BASE, "tech": ["GWG_K45Max", "GWG_Wasserdruck"], "action": "drop", "devices": ["GWG_A"]}
    found = confirmed.parse({"finding": [entry]})
    assert [f.tech for f in found] == ["GWG_K45Max", "GWG_Wasserdruck"]
    assert all(f.action == "drop" and f.devices == ("GWG_A",) for f in found)
    cat = _catalog()
    cat.use_findings(found)
    assert not {"GWG_K45Max", "GWG_Wasserdruck"} & set(_by_tech(cat.events_for("GWG_A")))


@pytest.mark.parametrize(
    ("over", "message"),
    [
        ({"tech": ["GWG_K45Max"], "action": "change", "set": {"access_type": 1}}, "drop and confirm only"),
        ({"tech": [], "action": "drop"}, "non-empty list"),
        ({"tech": ["GWG_K45Max", "GWG_K45Max"], "action": "drop"}, "an id twice"),
    ],
)
def test_a_list_of_tech_ids_is_refused_where_it_does_not_fit(over, message):
    entry = {**_BASE, "devices": ["GWG_A"], **over}
    with pytest.raises(FindingError, match=message):
        confirmed.parse({"finding": [entry]})


def test_drop_removes_the_datapoint():
    cat = _catalog()
    cat.use_findings([_finding(tech="GWG_K45Max", action="drop", devices=["GWG_A"])])
    assert "GWG_K45Max" not in _by_tech(cat.events_for("GWG_A"))
    assert "GWG_K45Max" in _by_tech(cat.events_for("GWG_B"))


@pytest.mark.parametrize(
    ("over", "message"),
    [
        ({"tech": "GWG_K46Red", "action": "add", "devices": ["GWG_B"]}, "already on GWG_B"),
        ({"tech": "GWG_K46Red", "action": "change", "devices": ["GWG_A"], "set": {"access_type": 1}}, "not on GWG_A"),
        ({"tech": "GWG_Nothing", "action": "add", "devices": ["GWG_A"]}, "define it instead"),
        ({"tech": "GWG_K46Red", "action": "confirm", "devices": ["GWG_A"]}, "not on GWG_A"),
        (
            {"tech": "GWG_K46Red", "action": "define", "devices": ["GWG_A"], "name": _NAME, "set": _DEFINE},
            "add it from there",
        ),
        (
            {"tech": "GWG_K45Max", "action": "define", "devices": ["GWG_A"], "name": _NAME, "set": _DEFINE},
            "already on GWG_A",
        ),
        (
            {
                "tech": "GWG_New",
                "action": "define",
                "devices": ["GWG_A"],
                "name": _NAME,
                "set": {**_DEFINE, "address": 0x0C},
            },
            "GWG_K45Max on the same bytes",
        ),
        ({"tech": "GWG_K46Red", "action": "add", "devices": ["GWG_A"], "group_of": "GWG_Nothing"}, "group_of"),
        ({"tech": "GWG_K46Red", "action": "add", "devices": ["GWG_A"], "from": "GWG_Z"}, "not in the export"),
    ],
)
def test_findings_that_do_not_fit_the_export_are_refused(over, message):
    cat = _catalog()
    cat.use_findings([_finding(**over)])
    with pytest.raises(FindingError, match=message):
        cat.events_for(over["devices"][0])


def test_add_needs_from_when_the_export_disagrees():
    cat = _catalog()
    other = _ev("4", "GWG_K46Red", 0x0E, access_type=3, fc_write="EEPROM_WRITE")
    cat._events["4"] = other
    cat._links["dpC"] = ["4"]
    cat.devices["GWG_C"] = "dpC"
    cat.use_findings([_finding(tech="GWG_K46Red", action="add", devices=["GWG_A"])])
    with pytest.raises(FindingError, match="name one with from"):
        cat.events_for("GWG_A")
    cat.use_findings([_finding(tech="GWG_K46Red", action="add", devices=["GWG_A"], **{"from": "GWG_C"})])
    assert _by_tech(cat.events_for("GWG_A"))["GWG_K46Red"].address == 0x0E


def test_confirm_labels_the_row_and_changes_nothing():
    cat = _catalog()
    cat.use_findings([_finding(tech="GWG_K45Max", action="confirm", devices=["GWG_A"], checked="write")])
    on_a = _by_tech(cat.events_for("GWG_A"))["GWG_K45Max"]
    assert on_a.confirmed == ("seen on the unit [write on 0x2053 HW=0x01 SW=0x1E; SoulSolistice/esphome_vitohome#35]",)
    assert (on_a.address, on_a.access_type, on_a.added) == (0x0C, 3, False)


def test_define_builds_the_datapoint_in_the_catalog_language():
    cat = _catalog()
    cat.use_findings(
        [
            _finding(
                tech="GWG_New",
                action="define",
                devices=["GWG_A"],
                name=_NAME,
                set={**_DEFINE, "unit": "K"},
                group_of="GWG_K45Max",
            )
        ]
    )
    new = _by_tech(cat.events_for("GWG_A"))["GWG_New"]
    assert (new.name, new.address, new.unit, new.added) == ("Wert neu", 0x40, "K", True)
    assert "(not in the Vitosoft export)" in new.confirmed[0]
    assert "GWG_New" not in _by_tech(cat.events_for("GWG_B"))
    cat.culture = "en"
    assert _by_tech(cat.events_for("GWG_A"))["GWG_New"].name == "New value"


def test_an_added_entity_takes_the_name_qualifier_alone():
    cat = _catalog()
    cat._events["2"].name = "New value"
    cat._events["2"].access_type = 1  # a sensor, like the defined one
    cat.culture = "en"
    cat.use_findings([_finding(tech="GWG_New", action="define", devices=["GWG_A"], name=_NAME, set=_DEFINE)])
    out = generate(cat, "GWG_A", "full", None, None, protocol=PROTO_GWG)
    assert 'name: "New value (GWG_New)"' in out
    assert 'name: "New value"' in out


def test_an_unknown_device_is_a_warning():
    cat = _catalog()
    warnings = cat.use_findings([_finding(tech="GWG_K45Max", action="drop", devices=["GWG_Z"])])
    assert warnings == ["finding GWG_K45Max: device GWG_Z is not in this export, skipped"]


# --- rendering ---------------------------------------------------------------


def test_big_endian_finding_renders_rotatebytes_with_its_label():
    cat = _catalog()
    cat._events["1"].conversion = "MultOffset"
    cat._events["1"].conv_factor = 0.0052
    cat._events["1"].conv_offset = -0.65
    cat.use_findings([_finding(tech="GWG_Wasserdruck", action="change", devices=["GWG_B"], set={"big_endian": True})])
    out_b = generate(cat, "GWG_B", "full", None, None, protocol=PROTO_GWG)
    out_a = generate(cat, "GWG_A", "full", None, None, protocol=PROTO_GWG)
    block = out_b.split("id: GWG_Wasserdruck")[1].split("- platform")[0]
    assert "converter: rotatebytes" in block
    assert "multiply: 0.0052" in block
    assert "# confirmed: seen on the unit [read on 0x2053 HW=0x01 SW=0x1E; SoulSolistice/esphome_vitohome#35]" in out_b
    assert "Hardware findings: 1 entity carries" in out_b
    assert "rotatebytes" not in out_a
    assert "# confirmed:" not in out_a


def test_big_endian_on_a_one_byte_row_is_refused():
    cat = _catalog()
    cat.use_findings([_finding(tech="GWG_K45Max", action="change", devices=["GWG_A"], set={"big_endian": True})])
    with pytest.raises(FindingError, match="big_endian needs"):
        generate(cat, "GWG_A", "full", None, None, protocol=PROTO_GWG)


# --- the shipped findings ------------------------------------------------------


def _catalog_tokens():
    tokens = set()
    for path in glob.glob(os.path.join(_REPO_ROOT, "example", "catalogs", "*", "index.csv")):
        with open(path, encoding="utf-8", newline="") as fh:
            tokens |= {row["token"] for row in csv.DictReader(fh) if row.get("file")}
    return tokens


def test_shipped_findings_name_catalogued_devices():
    tokens = _catalog_tokens()
    assert tokens
    for f in confirmed.load():
        for dev in f.devices:
            assert dev in tokens, f"{f.tech}: {dev} has no committed catalog"


def test_shipped_findings_are_in_their_catalogs():
    """Every change and add shows up as a `# confirmed:` line in the committed
    catalog of each device it names (the catalogs are regenerated with it)."""
    for f in confirmed.load():
        if f.action == "drop":
            for dev in f.devices:
                for lang in ("de", "en"):
                    path = os.path.join(_REPO_ROOT, "example", "catalogs", lang, f"{dev.lower()}.yaml")
                    with open(path, encoding="utf-8") as fh:
                        assert f"id: {f.tech}\n" not in fh.read(), f"{f.tech} still in {lang}/{dev.lower()}.yaml"
            continue
        for dev in f.devices:
            for lang in ("de", "en"):
                stem = dev.lower()
                with open(os.path.join(_REPO_ROOT, "example", "catalogs", lang, f"{stem}.yaml"), encoding="utf-8") as fh:
                    text = fh.read()
                assert f"# confirmed: {f.label}" in text, f"{f.tech} not regenerated into {lang}/{stem}.yaml"
                assert f"id: {f.tech}" in text
