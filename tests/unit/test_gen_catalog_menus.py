"""Datapoints a device carries only in its own Vitosoft menu tree (gen_catalog).

Vitosoft attaches datapoints to a device in two places: the direct
ecnDataPointTypeEventTypeLink table and the device's menu tree
(ecnEventTypeGroup rows carrying its DataPointTypeId). 150 devices of the 2026
export have datapoints only in the menus -- 2,448 at an address the device has
no other datapoint for -- so Catalog.events_for() takes both unless
--no-menu-datapoints is given.

    python -m pytest tests/unit/test_gen_catalog_menus.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog.loader import load_catalog  # noqa: E402
from vitocatalog.models import Event  # noqa: E402
from vitocatalog.transforms import _is_lon_input, _profile_keep  # noqa: E402

_FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


@pytest.fixture
def catalog():
    cat = load_catalog(_FIXTURE_DIR)
    # VOther_01 (dp 102) has no direct datapoint; give it a menu that carries
    # VTestHO1_99's event 6 (0x6300) and event 1 twice over two groups.
    cat._groups["9100"] = {"path": "VOther_01~10_Bedienung", "dp": "102", "order": 1}
    cat._groups["9101"] = {"path": "ecnsysEventTypeGroup~VOther_01~Diagnose", "dp": "102", "order": 2}
    cat._glinks.setdefault("6", []).append(("9100", 1))
    cat._glinks.setdefault("1", []).extend([("9100", 2), ("9101", 1)])
    return cat


def test_menu_only_datapoints_are_included(catalog):
    events = catalog.events_for("VOther_01")
    assert sorted(e.id for e in events) == ["1", "6"]  # each event once
    assert {e.address for e in events} == {0x0800, 0x6300}


def test_switch_turns_them_off(catalog):
    catalog.menu_datapoints = False
    assert catalog.events_for("VOther_01") == []


def test_menus_never_duplicate_direct_links(catalog):
    # VTestHO1_99's menus only hold events it is linked to directly anyway.
    with_menus = [e.id for e in catalog.events_for("VTestHO1_99")]
    catalog.menu_datapoints = False
    direct = [e.id for e in catalog.events_for("VTestHO1_99")]
    assert with_menus == direct
    assert len(set(with_menus)) == len(with_menus)


def test_direct_links_come_first_in_their_own_order(catalog):
    import copy

    # A datapoint no device links directly, reachable only through a menu.
    extra = copy.copy(catalog._events["6"])
    extra.id = "99"
    catalog._events["99"] = extra
    catalog._groups["9200"] = {"path": "VTestHO1_99~30_Extra", "dp": "101", "order": 3}
    catalog._glinks["99"] = [("9200", 1)]
    ids = [e.id for e in catalog.events_for("VTestHO1_99")]
    catalog.menu_datapoints = False
    direct = [e.id for e in catalog.events_for("VTestHO1_99")]
    assert ids[: len(direct)] == direct
    assert ids[len(direct) :] == ["99"]


def test_another_devices_menu_does_not_leak(catalog):
    # Event 6 sits in VOther_01's menu; VTestHO1_99 must not gain anything from it.
    catalog.menu_datapoints = False
    direct = {e.id for e in catalog.events_for("VTestHO1_99")}
    catalog.menu_datapoints = True
    assert {e.id for e in catalog.events_for("VTestHO1_99")} == direct


# --- LON inputs and the profiles -----------------------------------------------------------


def _lon(token):
    return Event(
        id="l",
        name=token,
        address=0xA300,
        conversion="Div2",
        access_type=3,
        block_length=2,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        token=f"{token}~0xA300",
        fc_read="Virtual_READ",
    )


@pytest.mark.parametrize(
    ("token", "is_input"),
    [
        ("nviBoilerCmd_BLR_value", True),
        ("nviHCC1_SpaceSetpt", True),
        ("nvoAlarm_Fehlercode", False),  # output: the controller's own state
        ("nciConfig", False),  # configuration
        ("ddcRPCnviErr", False),  # not a LON input token
        ("nvidia", False),  # lower-case after the prefix
    ],
)
def test_lon_input_detection(token, is_input):
    assert _is_lon_input(_lon(token)) is is_input


def test_lon_inputs_only_in_full():
    ev = _lon("nviBoilerCmd_BLR_value")
    assert _profile_keep(ev, "full")
    assert not _profile_keep(ev, "standard")
    assert not _profile_keep(ev, "minimal")
    assert _profile_keep(_lon("nvoAlarm_Fehlercode"), "standard")
