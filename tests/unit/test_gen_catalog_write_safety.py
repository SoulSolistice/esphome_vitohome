"""What a generated catalog lets Home Assistant write.

* Every entity that writes the controller (number, select, switch, text)
  carries ``entity_category: config``, like the action buttons.
* An action that resets settings or codings to their factory state, or deletes
  stored data, is emitted commented out under a DESTRUCTIVE note.
* A GWG catalog does not claim the unit has no Optolink datapoints when it
  only drops the 16-bit system fault archive.

Run:
    python -m pytest tests/unit/test_gen_catalog_write_safety.py -q
"""

import os
import sys
from types import SimpleNamespace

import pytest
import yaml

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402
from vitocatalog.special import _schaltzeiten_lines  # noqa: E402


def _event(**overrides):
    base = {
        "id": "1",
        "name": "Test Parameter",
        "address": 0x2500,
        "conversion": "",
        "access_type": 3,
        "block_length": 0,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "0",
        "upper": "100",
        "stepping": "1",
        "enum_type": False,
        "unit": "",
        "tech": "test_parameter",
        "token": "Test~0x2500",
        "fc_read": "",
        "fc_write": "",
        "conv_factor": None,
        "conv_offset": None,
        "block_factor": None,
        "values": [],
    }
    base.update(overrides)
    return SimpleNamespace(**base)


def _emit(**overrides):
    platform, lines = gc.emit_entity(_event(**overrides), "full")
    return platform, lines


# --- entity_category: config ------------------------------------------------


def test_a_writable_number_is_config():
    platform, lines = _emit()
    assert platform == "number"
    assert "  entity_category: config" in lines


def test_a_writable_select_is_config():
    def value(raw, label):
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

    values = [value(0, "Aus"), value(1, "Ein"), value(2, "Auto")]
    platform, lines = _emit(enum_type=True, values=values, lower="", upper="", stepping="")
    assert platform == "select"
    assert "  entity_category: config" in lines


def test_a_read_only_value_is_not_config():
    platform, lines = _emit(access_type=1)
    assert platform == "sensor"
    assert not any("entity_category: config" in ln for ln in lines)


def test_switching_times_are_config():
    lines = _schaltzeiten_lines({"name": "HK1 Schaltzeit Montag", "address": 0x2000}, "hk1_mo")
    assert "  entity_category: config" in lines


# --- destructive actions ----------------------------------------------------


@pytest.mark.parametrize(
    ("tech", "name"),
    [
        ("BedienparameterA1M1FunktionReset", "Grundeinstellung Bedienparameter A1M1"),
        ("Konfi_FunktionReset_GWG", "Anlieferzustand Codierungen"),
        ("WPR3_Reset_Parameter_ALZ", "Alle Parameter auf ALZ"),
        ("valmRPCClearErrorHistoryTable", "Fehlerhistorie Löschen"),
        ("vlogRPCClearMemberList", "Teilnehmerliste loeschen"),
        ("vlogRPCDelLONCh00", "Del DP 00"),
        ("RPCLoeschenTrendingWILOKanal_00", "Löschen Trending WILO-Kanal 00"),
        ("RPCResetCounterLebenszeichen", "Rücksetzen Zähler Lebenszeichen"),
        ("anything", "Delete the error history"),
    ],
)
def test_resets_and_deletions_are_destructive(tech, name):
    assert gc._is_destructive_action(SimpleNamespace(tech=tech, name=name))


@pytest.mark.parametrize(
    ("tech", "name"),
    [
        ("RPCWink", "Wink"),
        ("KesselfolgeFunktionNext", "Nächste Kesselfolge"),
        ("KesselfolgelFunktionInit", "Grundstellung Kesselfolge"),
        ("WPR3_Konfi_FunktionNeustart", "Neustart Regelung"),
        ("RPCLesenFehlerhistorie", "Lesen Eintrag Fehlerhistorientabelle"),
        ("RPCServicePIN", "Servicepin-Message senden"),
    ],
)
def test_other_actions_are_not_destructive(tech, name):
    assert not gc._is_destructive_action(SimpleNamespace(tech=tech, name=name))


def _rpc_catalog(rows):
    raw_events, access = {}, {}
    for eid, (token, access_type, prefix, block) in rows.items():
        raw_events[eid] = {"Address": token, "Name": token.split("~")[0], "Type": access_type}
        access[token] = {
            "address": token.split("~")[1],
            "block_length": block,
            "byte_length": block,
            "byte_position": "0",
            "conversion": "NoConversion",
            "fc_read": "Remote_Procedure_Call",
            "fc_write": "Remote_Procedure_Call",
            "prefix_read": prefix,
            "prefix_write": prefix,
            "parameter_type": "Array" if prefix else "Byte",
        }
    return gc.Catalog({"VRpcTest_01": "dp1"}, {"dp1": "VRpcTest"}, {"dp1": list(rows)}, raw_events, {}, {}, access, [], {})


def _generate(rows):
    return gc.generate(_rpc_catalog(rows), "VRpcTest_01", "full", None, None, emit_device_id=False, emit_error_history=False)


def test_a_destructive_action_is_commented_out_and_not_counted():
    out = _generate(
        {
            "1": ("valmRPCClearErrorHistoryTable~0xA029", "3", "", "0"),
            "2": ("RPCWink~0xA00C", "3", "", "0"),
        }
    )
    buttons = out.split("\nbutton:\n", 1)[1]
    assert "\n  - platform: vitohome\n    type: action" in buttons
    assert "  # DESTRUCTIVE: this procedure resets settings" in buttons
    assert "  # - platform: vitohome" in buttons
    assert "  #   address: 0xA029" in buttons
    assert "Actions: 1 procedure " in out
    assert "Commented out: 1 procedure " in out
    assert "the `button:` line is commented out" not in out
    # The commented action is not an entity.
    assert yaml.safe_load(out)["button"] == [
        {
            "platform": "vitohome",
            "type": "action",
            "name": "RPCWink",
            "id": "RPCWink",
            "address": 0xA00C,
            "access": "rpc",
            "entity_category": "config",
            "disabled_by_default": True,
        }
    ]


def test_a_section_of_destructive_actions_only_is_a_comment():
    # An empty `button:` key would be null, which ESPHome rejects.
    out = _generate({"1": ("valmRPCClearErrorHistoryTable~0xA029", "3", "", "0")})
    assert "\nbutton:\n" not in out
    assert "\n# button:\n" in out
    assert "the `button:` line is commented out" in out
    assert "button" not in (yaml.safe_load(out) or {})


# --- GWG header ---------------------------------------------------------------


def _gwg_catalog():
    sensor = gc.Event(
        id="1",
        name="Kesseltemperatur",
        address=0x0A,
        conversion="Div10",
        access_type=1,
        block_length=2,
        byte_length=2,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        tech="Kesseltemperatur",
        token="Kesseltemperatur~0x0A",
        fc_read="Virtual_READ",
    )
    history = gc.Event(
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
        fc_read="Virtual_READ",
    )
    cat = gc.Catalog(
        types={"TestDev": "dp1"},
        dp_name={"dp1": "Test Device"},
        links={"dp1": ["1", "2"]},
        raw_events={},
        vlinks={},
        raw_values={},
        access={},
        ident_rows=[
            {"token": "TestDev", "ident": 0x20CB, "ext": 0x0148, "extt": 0x0148, "f0": None, "f0t": None, "error_type": "1"}
        ],
        textmap={},
    )
    cat._events = {"1": sensor, "2": history}
    return cat


def test_gwg_header_names_the_dropped_archive_only():
    out = gc.generate(_gwg_catalog(), "TestDev", "full", None, None, protocol="GWG")
    assert "address: 0x000A" in out
    assert "The generic system fault archive (0x7507..0x7558) is NOT emitted" in out
    assert "NO Optolink-reachable" not in out
