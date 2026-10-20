"""Remote procedure call readings in scripts/gen_catalog.py.

Vitosoft marks two different things FCRead Remote_Procedure_Call: READINGS --
a procedure answering with a value, called with a parameter (PrefixRead) or
without one -- and PROCEDURES with an effect (a reset, clearing a history). Only
readings may become entities, and only under P300, which has the telegram. They
carry `access: rpc`, and `prefix:` when they take a parameter; since one address
can hold a whole list, the prefix is part of the datapoint's identity. Readings
that only a LON module fills are left to `--profile full`, like the LON inputs.

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
    """One P300 device: three entries of the LON participant list, one reading
    without a parameter, a reset procedure that carries a PrefixRead, a
    parameterless procedure, and a read-only delete with no answer."""
    rows = {
        # eid: (token, access type, PrefixRead, BlockLength)
        "1": ("Teilnehmerliste_LON_00~0xA010", "1", "00", "6"),
        "2": ("Teilnehmerliste_LON_01~0xA010", "1", "01", "6"),
        "3": ("Teilnehmerliste_LON_15~0xA010", "1", "0F", "6"),
        "4": ("KesselfolgeIst~0xA104", "1", "", "4"),
        "5": ("BedienparameterA1M1FunktionReset~0xA051", "2", "00", "1"),
        "6": ("valmRPCClearErrorHistoryTable~0xA029", "3", "", "0"),
        "7": ("mBusRPCDeleteReadTableEM2~0xA093", "1", "01", "0"),
    }
    raw_events, access = {}, {}
    for eid, (token, access_type, prefix, block) in rows.items():
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
            "parameter_type": "Int",
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


def test_full_emits_every_reading_and_no_procedure():
    out = _generate("full")
    assert out.count("    access: rpc  #") == 4
    for prefix in ("0x00", "0x01", "0x0F"):
        assert f"prefix: [{prefix}]" in out
    # The three list entries share 0xA010 and differ only in the prefix; none folds.
    assert out.count("address: 0xA010") == 3
    # The reading without a parameter carries no prefix line.
    block = out[out.index("address: 0xA104") :].split("\n\n", 1)[0]
    assert "access: rpc" in block and "prefix:" not in block
    # No procedure is polled: the reset carries a PrefixRead but writes, the clear
    # writes, and the delete answers nothing.
    for address in ("0xA051", "0xA029", "0xA093"):
        assert address not in out
    assert "Remote procedure readings: 4 " in out


@pytest.mark.parametrize("profile", ["standard", "minimal"])
def test_standard_keeps_the_units_own_readings_and_leaves_lon_to_full(profile):
    out = _generate(profile)
    assert "0xA010" not in out
    assert "Remote procedure readings omitted: 3 " in out
    if profile == "standard":
        assert out.count("    access: rpc  #") == 1
        assert "address: 0xA104" in out


def test_kw_emits_none_of_them():
    out = _generate("full", protocol="KW")
    assert "access: rpc" not in out
    assert "0xA010" not in out
    assert "Remote procedure readings omitted" not in out
