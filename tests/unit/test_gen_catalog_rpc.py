"""Remote procedure call readings in scripts/gen_catalog.py.

Vitosoft marks two different things FCRead Remote_Procedure_Call: READINGS --
a procedure answering with a value, called with a parameter (PrefixRead) or
without one -- and PROCEDURES with an effect (a reset, clearing a history).
Under P300, which has the telegram, readings become sensors with `access: rpc`
(and `prefix:` when they take a parameter; since one address can hold a whole
list, the prefix is part of the datapoint's identity), and procedures become
`type: action` buttons, disabled by default under a warning. Readings that only
a LON module fills are left to `--profile full`, like the LON inputs.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_gen_catalog_rpc.py -q
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


def _ev(
    prefix="00",
    access_type=1,
    block_factor=None,
    block_length=6,
    fc_read="Remote_Procedure_Call",
    address=0xA010,
    token="Teilnehmerliste_LON_00~0xA010",
):
    return gc.Event(
        id="t",
        name="t",
        address=address,
        conversion="NoConversion",
        access_type=access_type,
        block_length=block_length,
        byte_length=2 if block_length else 0,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        fc_read=fc_read,
        fc_write="Remote_Procedure_Call",
        prefix_read=prefix,
        block_factor=block_factor,
        token=token,
    )


# --- what counts as a reading -------------------------------------------------


def test_a_prefixed_read_only_single_record_is_a_reading():
    ev = _ev()
    assert gc._is_rpc_reading(ev, gc.PROTO_P300)
    assert gc._is_reachable(ev, gc.PROTO_P300)
    assert gc._access_for(ev, gc.PROTO_P300) == "rpc"
    assert gc._rpc_prefix(_ev("00 01 02")) == [0x00, 0x01, 0x02]


def test_a_reading_may_take_no_parameter():
    # KesselfolgeIst and its siblings on a cascade controller: read-only, a 4-byte
    # answer, no PrefixRead.
    ev = _ev(prefix="", block_length=4, address=0xA104, token="KesselfolgeIst~0xA104")
    assert gc._is_rpc_reading(ev, gc.PROTO_P300)
    assert gc._rpc_prefix(ev) == []
    assert gc._access_for(ev, gc.PROTO_P300) == "rpc"


@pytest.mark.parametrize(
    "ev,why",
    [
        (_ev(access_type=2), "writable: a procedure (BedienparameterA1M1FunktionReset has a PrefixRead too)"),
        (_ev(prefix="", access_type=3), "read+write without a parameter: valmRPCClearErrorHistoryTable"),
        (_ev(prefix="01", block_length=0), "no answer: mBusRPCDeleteReadTableEM2 is marked read-only"),
        (_ev(block_factor=30), "an array: one call per index, not one entity"),
        (_ev(prefix="0"), "unparsable parameter"),
        (_ev(prefix="zz"), "unparsable parameter"),
        (_ev(prefix="0001020304"), "longer than the component accepts"),
        (_ev(fc_read="Virtual_READ"), "not an RPC at all"),
    ],
)
def test_everything_else_is_not_a_reading(ev, why):
    assert not gc._is_rpc_reading(ev, gc.PROTO_P300), why
    assert gc._access_for(ev, gc.PROTO_P300) != "rpc", why


@pytest.mark.parametrize("protocol", [gc.PROTO_KW, gc.PROTO_GWG])
def test_only_p300_has_the_telegram(protocol):
    ev = _ev()
    assert not gc._is_rpc_reading(ev, protocol)
    assert not gc._is_reachable(ev, protocol)
    assert gc._access_for(ev, protocol) == ""


@pytest.mark.parametrize(
    "ev,lon",
    [
        (_ev(), True),  # the LON participant list at 0xA010
        (_ev(prefix="", block_length=11, token="Vitocom_Node~0xA010"), True),  # LON identity, same procedure
        (_ev(prefix="", block_length=1, address=0xA042, token="vlonRPCGetBinding~0xA042"), True),
        (_ev(prefix="", block_length=3, address=0xA043, token="ddcRPCnviError_01~0xA043"), True),  # a LON input
        (_ev(prefix="", block_length=4, address=0xA104, token="KesselfolgeIst~0xA104"), False),
        (_ev(prefix="", block_length=2, address=0xA090, token="mBusRPCCheckHWEM1~0xA090"), False),
    ],
)
def test_lon_bound_readings_are_told_apart_from_the_units_own(ev, lon):
    assert gc._is_lon_bound_rpc(ev) is lon
    assert gc._profile_keep(ev, "standard") is not lon
    assert gc._profile_keep(ev, "full")


# --- end to end ---------------------------------------------------------------


def _catalog():
    """One P300 device, shaped like the export: three entries of the LON
    participant list, one reading without a parameter, a reset procedure with a
    parameter, a parameterless procedure, a read-only delete with no answer, and
    a setter that needs a value."""
    rows = {
        # eid: (token, access type, Prefix, BlockLength, Parameter, borders)
        "1": ("Teilnehmerliste_LON_00~0xA010", "1", "00", "6", "Array", ""),
        "2": ("Teilnehmerliste_LON_01~0xA010", "1", "01", "6", "Array", ""),
        "3": ("Teilnehmerliste_LON_15~0xA010", "1", "0F", "6", "Array", ""),
        "4": ("KesselfolgeIst~0xA104", "1", "", "4", "Array", ""),
        "5": ("BedienparameterA1M1FunktionReset~0xA051", "2", "00", "1", "Array", ""),
        "6": ("valmRPCClearErrorHistoryTable~0xA029", "3", "", "0", "Byte", ""),
        "7": ("mBusRPCDeleteReadTableEM2~0xA093", "1", "01", "0", "Byte", ""),
        "8": ("RPCLesenFehlertabelle~0xA0B0", "3", "", "1", "Byte", "74"),
    }
    raw_events, access = {}, {}
    for eid, (token, access_type, prefix, block, parameter, upper) in rows.items():
        raw_events[eid] = {"Address": token, "Name": token.split("~")[0], "Type": access_type}
        access[token] = {
            "address": token.split("~")[1],
            "block_length": block,
            "byte_length": "2" if block == "6" else block,
            "byte_position": "0",
            "conversion": "NoConversion",
            "fc_read": "Remote_Procedure_Call",
            "fc_write": "Remote_Procedure_Call",
            "prefix_read": prefix,
            "prefix_write": prefix,
            "parameter_type": parameter,
            "lower_border": "0" if upper else "",
            "upper_border": upper,
        }
    return gc.Catalog(
        {"VRpcTest_01": "dp1"},
        {"dp1": "VRpcTest"},
        {"dp1": list(rows)},
        raw_events,
        {},
        {},
        access,
        [],
        {},
    )


def _generate(profile, protocol="P300"):
    return gc.generate(
        _catalog(), "VRpcTest_01", profile, None, None, emit_device_id=False, emit_error_history=False, protocol=protocol
    )


def test_full_emits_every_reading_and_every_action():
    out = _generate("full")
    sensors, buttons = out.split("\nbutton:\n", 1)
    assert sensors.count("    access: rpc  #") == 4
    for prefix in ("0x00", "0x01", "0x0F"):
        assert f"prefix: [{prefix}]  # Vitosoft PrefixRead" in sensors
    # The three list entries share 0xA010 and differ only in the prefix; none folds.
    assert sensors.count("address: 0xA010") == 3
    # The reading without a parameter carries no prefix line.
    block = sensors[sensors.index("address: 0xA104") :].split("\n\n", 1)[0]
    assert "access: rpc" in block and "prefix:" not in block
    # Both procedures are buttons, disabled, under the warning; the reset carries
    # its parameter from PrefixWrite.
    assert buttons.count("    type: action") == 2
    assert buttons.count("# WARNING: pressing this RUNS a procedure") == 2
    assert buttons.count("disabled_by_default: true") == 2
    assert "prefix: [0x00]  # Vitosoft PrefixWrite 00" in buttons
    assert "address: 0xA029" in buttons and "address: 0xA051" in buttons
    # The read-only delete answers nothing and is neither; the setter needs a value.
    assert "0xA093" not in out
    assert "0xA0B0" not in out
    assert "Remote procedure readings: 4 " in out
    assert "Actions: 2 procedures" in out


@pytest.mark.parametrize("profile", ["standard", "minimal"])
def test_standard_keeps_the_units_own_readings_and_every_action(profile):
    out = _generate(profile)
    assert "0xA010" not in out
    assert "Remote procedure readings omitted: 3 " in out
    if profile == "standard":
        assert "address: 0xA104" in out
        assert out.count("    type: action") == 2
    else:
        # minimal keeps values; an action is not one.
        assert "    type: action" not in out


@pytest.mark.parametrize(
    "ev,action",
    [
        (_ev(prefix="", access_type=3, block_length=0), True),  # clear the fault history
        (_ev(prefix="00", access_type=2, block_length=1), True),  # a circuit reset (Array, no borders)
        (_ev(prefix="", access_type=2, block_length=0), True),  # clear the LON participant list
        (_ev(prefix="", access_type=1, block_length=0), False),  # read-only: not a procedure
        (_ev(prefix="", access_type=3, block_length=2), False),  # read+write that answers: a setter
        (_ev(prefix="", access_type=2, block_length=4), False),  # a 4-byte value: RPCSetImpulseCounter
        (_ev(prefix="", access_type=2, block_factor=30), False),  # an array
        (_ev(prefix="zz", access_type=3, block_length=0), False),  # unparsable parameter
    ],
)
def test_what_counts_as_an_action(ev, action):
    ev.parameter_type = "Array"
    ev.prefix_write = ev.prefix_read
    assert gc._is_rpc_action(ev, gc.PROTO_P300) is action
    assert not gc._is_rpc_action(ev, gc.PROTO_KW)


def test_a_setter_with_borders_or_a_unit_is_not_an_action():
    ev = _ev(prefix="", access_type=2, block_length=1)
    ev.parameter_type = "Array"
    assert gc._is_rpc_action(ev)
    ev.upper = "74"
    assert not gc._is_rpc_action(ev)
    ev.upper, ev.unit_token = "", "ecnUnit.Sekunden"
    assert not gc._is_rpc_action(ev)


def test_prefixwrite_may_be_written_with_0x():
    ev = _ev(prefix="")
    ev.prefix_write = "0x0E"
    assert gc._rpc_prefix(ev, "prefix_write") == [0x0E]


# --- the heat-pump fault history: an RPC array, fanned out per record ----------


def _heat_pump_catalog(extra=()):
    """A P300 heat pump: one ordinary reading (so the system archive is emitted at
    all), the WPRError fault buffer -- 30 x 8 bytes behind one procedure at
    0xA801 -- and optionally more access rows."""
    rows = [
        ("Aussentemperatur~0x0101", "1", "Virtual_READ", "", "2", ""),
        ("ecnsysEventType~WPRError", "1", "Remote_Procedure_Call", "", "240", "30"),
        *extra,
    ]
    raw_events, access, ids = {}, {}, []
    for n, (token, access_type, fc, prefix, block, factor) in enumerate(rows, start=1):
        eid = str(n)
        ids.append(eid)
        raw_events[eid] = {"Address": token, "Name": token.split("~")[0], "Type": access_type}
        access[token] = {
            "address": "0x0101" if token.startswith("Aussen") else "0xA801" if "WPR" in token else "0xA821",
            "block_length": block,
            "byte_length": block,
            "byte_position": "0",
            "block_factor": factor,
            "conversion": "Div10" if token.startswith("Aussen") else "NoConversion",
            "fc_read": fc,
            "fc_write": "Remote_Procedure_Call" if fc != "Virtual_READ" else "Virtual_WRITE",
            "prefix_read": prefix,
            "parameter_type": "Array",
        }
    return gc.Catalog({"VHeatPump_01": "dp1"}, {"dp1": "VHeatPump"}, {"dp1": ids}, raw_events, {}, {}, access, [], {})


def _heat_pump(protocol="P300", extra=()):
    return gc.generate(
        _heat_pump_catalog(extra), "VHeatPump_01", "standard", None, None, emit_device_id=False, protocol=protocol
    )


def test_rpc_fault_history_fans_out_one_slot_per_record():
    ev = _ev(prefix="", block_length=240, block_factor=30, address=0xA801, token="ecnsysEventType~WPRError")
    assert gc._rpc_array_records(ev) == (30, 8)
    out = _heat_pump()
    # Thirty slots, all at the procedure's address, the record index as the parameter.
    assert out.count("address: 0xA801") == 30
    for i in (0, 1, 29):
        assert f"prefix: [0x{i:02X}]  # the record index" in out
    assert out.count("    type: raw") == 30
    assert out.count("    length: 8") == 30
    # The existing slot convention: the newest enabled, the other 29 disabled.
    newest = out[out.index("prefix: [0x00]") :].split("\n\n", 1)[0]
    assert "disabled_by_default" not in newest
    assert out.count("disabled_by_default: true") >= 29
    # Named like the system archive, tagged as the second archive it is.
    assert '"Letzter Fehler (WPR)"' in out and '"Fehler 30 (WPR)"' in out


def test_a_9_byte_wpr3_buffer_fans_out_too():
    ev = _ev(prefix="", block_length=270, block_factor=30, address=0xA801, token="ecnsysEventType~WPR3Error")
    assert gc._rpc_array_records(ev) == (30, 9)


@pytest.mark.parametrize(
    "ev",
    [
        # WPR3MldgsHistExtTn1: a prefix AND thirty records -- the order of the two is unknown.
        _ev(prefix="01", block_length=270, block_factor=30, address=0xA821, token="WPR3MldgsHistExtTn1~0xA821"),
        _ev(prefix="", block_length=240, block_factor=30, access_type=3),  # writable: not a buffer
        _ev(prefix="", block_length=241, block_factor=30),  # records do not divide the block
        _ev(prefix="", block_length=6, block_factor=1),  # not an array
    ],
)
def test_other_rpc_arrays_are_not_fanned_out(ev):
    assert gc._rpc_array_records(ev) is None


@pytest.mark.parametrize("protocol", ["KW", "GWG"])
def test_the_rpc_fault_history_needs_p300(protocol):
    assert "0xA801" not in _heat_pump(protocol)


def test_kw_emits_none_of_them():
    out = _generate("full", protocol="KW")
    assert "access: rpc" not in out
    assert "0xA010" not in out
    assert "Remote procedure readings omitted" not in out
