"""GWG access-mode handling in scripts/gen_catalog.py.

The Vitosoft export carries the GWG access mode in each event's ``FCRead``, and
for GWG that field is half the datapoint's identity: GWG's address space is per
access mode, so two datapoints can share an address and differ only in mode.
These tests pin the mapping, the inverted reachability rule, the read-only
demotion and the address guard -- and that none of it leaks into P300.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_gen_catalog_gwg_access.py -q
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


def _ev(fc_read="", address=0x01, access_type=3, fc_write="Virtual_WRITE"):
    return gc.Event(
        id="t",
        name="t",
        address=address,
        conversion="NoConversion",
        access_type=access_type,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        fc_read=fc_read,
        fc_write=fc_write,
    )


# --- the mapping itself -----------------------------------------------------


# Triple-sourced: the openv wiki's Protokoll-GWG "Telegramm Typen" table,
# vcontrold's <protocol name="GWG"> macros, and Vitosoft's FCRead names all
# agree on these seven. Drift here silently points every GWG entity at a
# different register file, so it is pinned rather than derived.
@pytest.mark.parametrize(
    "fc_read,access",
    [
        ("Physical_READ", "physical"),
        ("Virtual_READ", "virtual"),
        ("EEPROM_READ", "eeprom"),
        ("XRAM_READ", "xram"),
        ("Port_READ", "port"),
        ("BE_READ", "be"),
        ("KMBUS_EEPROM_READ", "kmbus_eeprom"),
    ],
)
def test_fcread_maps_to_access_mode(fc_read, access):
    assert gc._access_for(_ev(fc_read), gc.PROTO_GWG) == access
    # Case is not significant in the export.
    assert gc._access_for(_ev(fc_read.lower()), gc.PROTO_GWG) == access


def test_kmbus_ram_has_no_fcread():
    """0x33 is reachable by no Vitosoft datapoint and has no vcontrold macro.

    The enum still carries it (the wiki lists it), but nothing may generate it:
    if a future export introduces a name for it, this test failing is the
    intended signal to go verify the byte rather than trust the transcription.
    """
    assert "kmbus_ram" not in gc.FCREAD_TO_GWG_ACCESS.values()


def test_blank_fcread_emits_no_access_line():
    # Blank means "no access row"; the engine default is already physical, so
    # emitting nothing is both correct and less noise.
    assert gc._access_for(_ev(""), gc.PROTO_GWG) == ""


def test_access_never_emitted_for_non_gwg():
    for proto in (gc.PROTO_P300, gc.PROTO_KW):
        assert gc._access_for(_ev("Physical_READ"), proto) == ""
        assert gc._access_for(_ev("Virtual_READ"), proto) == ""


# --- reachability inverts between protocols ---------------------------------


def test_p300_reachability_unchanged():
    assert gc._is_reachable(_ev("Virtual_READ"), gc.PROTO_P300)
    assert gc._is_reachable(_ev(""), gc.PROTO_P300)
    # Everything else is unreachable through VS2's virtual read.
    for fc in ("Physical_READ", "EEPROM_READ", "GFA_READ", "Remote_Procedure_Call"):
        assert not gc._is_reachable(_ev(fc), gc.PROTO_P300)


def test_gwg_reachability_covers_every_access_mode():
    # The exact inversion: Physical_READ is unreachable on P300 and reachable
    # on GWG, where it is in fact the default mode.
    for fc in gc.FCREAD_TO_GWG_ACCESS:
        assert gc._is_reachable(_ev(fc), gc.PROTO_GWG)
    assert gc._is_reachable(_ev(""), gc.PROTO_GWG)


def test_gwg_still_drops_non_access_mode_reads():
    # KBUS_VIRTUAL_READ is a genuine K-bus tunnel -- no GWG access mode reaches
    # it (0.8% of GWG events in the real export).
    for fc in ("KBUS_VIRTUAL_READ", "Remote_Procedure_Call", "GFA_READ", "Virtual_MBUS"):
        assert not gc._is_reachable(_ev(fc), gc.PROTO_GWG)


# --- writes -----------------------------------------------------------------


@pytest.mark.parametrize(
    "fc_read,fc_write",
    [
        # The only two pairings that actually occur: 1082 and 280 datapoints
        # respectively across Vitosoft's 22 GWG device tokens.
        ("EEPROM_READ", "EEPROM_WRITE"),
        ("BE_READ", "BE_WRITE"),
    ],
)
def test_gwg_writable_when_mode_pairs(fc_read, fc_write):
    assert gc._is_writable(_ev(fc_read, fc_write=fc_write), gc.PROTO_GWG)


def test_gwg_write_requires_matching_read_mode():
    # `access:` is ONE option driving both directions, so a datapoint whose read
    # and write modes disagreed could not be expressed. Vitosoft never pairs
    # them across modes; this guards a future export that broke the rule.
    ev = _ev("EEPROM_READ", fc_write="BE_WRITE")
    assert not gc._is_writable(ev, gc.PROTO_GWG)


def test_gwg_write_requires_a_write_type_byte():
    # KMBUS has no write TYPE byte in the wiki table, and no FCWrite name maps
    # to it -- so a KMBUS datapoint is read-only however Vitosoft marks it.
    assert not gc._is_writable(_ev("KMBUS_EEPROM_READ", fc_write="KMBUS_EEPROM_WRITE"), gc.PROTO_GWG)
    assert "kmbus_eeprom" not in gc.FCWRITE_TO_GWG_ACCESS.values()
    assert "kmbus_ram" not in gc.FCWRITE_TO_GWG_ACCESS.values()


def test_gwg_unwritable_fcwrite_values():
    for fc in ("undefined", "", "KBUS_VIRTUAL_WRITE", "Remote_Procedure_Call"):
        assert not gc._is_writable(_ev("EEPROM_READ", fc_write=fc), gc.PROTO_GWG)


def test_gwg_respects_vitosoft_access_type():
    for at in (0, 1):
        assert not gc._is_writable(_ev("EEPROM_READ", access_type=at, fc_write="EEPROM_WRITE"), gc.PROTO_GWG)


def test_p300_write_rule_unchanged_by_gwg_table():
    ev = _ev("Virtual_READ", access_type=3, fc_write="Virtual_WRITE")
    assert gc._is_writable(ev, gc.PROTO_P300)
    # An EEPROM pairing is writable on GWG but must stay unwritable on P300.
    eeprom = _ev("EEPROM_READ", access_type=3, fc_write="EEPROM_WRITE")
    assert not gc._is_writable(eeprom, gc.PROTO_P300)


# --- protocol detection -----------------------------------------------------


@pytest.mark.parametrize(
    "token,expected",
    [
        ("GWG_VBES_00", gc.PROTO_GWG),
        ("GWG_BT2", gc.PROTO_GWG),
        ("HV_GWG", gc.PROTO_GWG),
        ("VScotHO1_72", gc.PROTO_P300),
        ("VTestHO1_99", gc.PROTO_P300),
    ],
)
def test_protocol_inferred_from_device_token(token, expected):
    assert gc._protocol_for_device(token) == expected


def test_explicit_protocol_overrides_token():
    assert gc._protocol_for_device("VScotHO1_72", gc.PROTO_GWG) == gc.PROTO_GWG
    assert gc._protocol_for_device("GWG_VBES_00", gc.PROTO_P300) == gc.PROTO_P300


def test_substring_alone_does_not_trigger_gwg():
    # Matching on underscore-delimited parts, not a bare substring, so a token
    # that merely contains the letters is not misclassified.
    assert gc._protocol_for_device("VGWGXX_01") == gc.PROTO_P300


# --- the fan-out paths ------------------------------------------------------
#
# generate() has three emission paths: the ordinary datapoint loop, the
# error-history fan-out (one block -> up to 10 slots) and the Schaltzeiten
# fan-out (one 56-byte block -> 7 weekday entities). Each `continue`s early, so
# a guard placed in the ordinary path alone does not cover the other two. That
# was already the cause of one shipped bug (every GWG catalog rejected by
# `esphome config` because the generic system archive's fixed 16-bit slots were
# emitted); these pin the same property for the Schaltzeiten path, which fans
# out to base + 6*8 and so can leave the 8-bit space even from a legal base.


def _schaltzeiten_ev(addr, fc_read="EEPROM_READ"):
    return gc.Event(
        id="sz",
        name="Schaltzeiten_A1M1_HK",
        address=addr,
        conversion="NoConversion",
        access_type=3,
        block_length=56,
        byte_length=56,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        block_factor=7,
        fc_read=fc_read,
        fc_write="EEPROM_WRITE",
        tech="Schaltzeiten_A1M1_HK",
        token=f"Schaltzeiten_A1M1_HK~0x{addr:04X}",
    )


def test_schaltzeiten_lines_carry_the_gwg_access_mode():
    # A GWG address is only a datapoint identity together with its mode, so the
    # weekday fan-out must emit `access:` like any other entity. Without it the
    # engine reads (and writes) these on its PHYSICAL default.
    ev = _schaltzeiten_ev(0x20)
    entry = gc._schaltzeiten_entries(ev)[0]
    body = "\n".join(gc._schaltzeiten_lines(entry, "sz_mo", gc._access_for(ev, gc.PROTO_GWG)))
    assert "access: eeprom" in body


def test_schaltzeiten_lines_omit_access_on_p300():
    # P300 has no access modes; emitting the key there would be rejected by the
    # hub's _final_validate.
    ev = _schaltzeiten_ev(0x2000)
    entry = gc._schaltzeiten_entries(ev)[0]
    body = "\n".join(gc._schaltzeiten_lines(entry, "sz_mo", gc._access_for(ev, gc.PROTO_P300)))
    assert "access:" not in body


def test_schaltzeiten_lines_default_has_no_access():
    # Back-compat: the 2-argument call used everywhere before GWG modes existed
    # must still produce the identical P300 entity.
    ev = _schaltzeiten_ev(0x2000)
    entry = gc._schaltzeiten_entries(ev)[3]
    assert gc._schaltzeiten_lines(entry, "sz_do") == gc._schaltzeiten_lines(entry, "sz_do", "")


@pytest.mark.parametrize(
    "base,fits",
    [
        (0x00, True),  # last day 0x30
        (0xCF, True),  # last day 0xFF exactly (0xCF + 6*8)
        (0xD0, False),  # last day 0x100 -- one past the space
        (0xF0, False),
    ],
)
def test_schaltzeiten_block_fits_the_8bit_space(base, fits):
    # The base can pass an address guard while the seventh weekday does not.
    last = base + (len(gc._WEEKDAYS) - 1) * gc._SCHALTZEITEN_RECORD_LEN
    assert (last <= 0xFF) is fits
