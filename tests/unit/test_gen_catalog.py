"""Unit tests for scripts/gen_catalog.py against the synthetic fixture.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_gen_catalog.py -q
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

_FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


@pytest.fixture(scope="module")
def catalog():
    return gc.load_catalog(_FIXTURE_DIR)


# --- parsing ----------------------------------------------------------------


def test_devices_discovered(catalog):
    assert "VTestHO1_99" in catalog.devices
    assert "VOther_01" in catalog.devices


def test_events_linked_to_device(catalog):
    events = catalog.events_for("VTestHO1_99")
    assert len(events) == 20
    # The unrelated device has no linked events.
    assert catalog.events_for("VOther_01") == []


def test_addresses_parsed_from_name(catalog):
    events = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    assert events["1"].address == 0x0800
    assert events["2"].address == 0x08A7
    assert events["6"].address == 0x6300


def test_access_types_and_conversions(catalog):
    events = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    assert events["1"].conversion == "Div10"
    assert events["1"].access_type == 1  # read-only
    assert events["5"].access_type == 3  # writable enum (Vitosoft Type 3 = read+write)
    assert events["6"].access_type == 3  # writable number
    assert events["15"].access_type == 2  # WRITE-ONLY reset register (Type 2)


def test_enum_values_attached(catalog):
    events = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    opts = gc._enum_options(events["5"])
    assert (0, "Standby") in opts
    assert (2, "Heizen") in opts


# --- platform routing -------------------------------------------------------


def _platform_of(catalog, event_id, profile="full"):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}[event_id]
    result = gc.emit_entity(ev, profile)
    return result[0] if result else None


def test_routing(catalog):
    assert _platform_of(catalog, "1") == "sensor"  # div10 temp
    assert _platform_of(catalog, "2") == "sensor"  # sec2hour counter
    assert _platform_of(catalog, "3") == "binary_sensor"  # bit field
    assert _platform_of(catalog, "4") == "text_sensor"  # read-only enum
    assert _platform_of(catalog, "5") == "select"  # writable enum
    assert _platform_of(catalog, "6") == "number"  # writable + borders
    assert _platform_of(catalog, "7") == "number"  # writable, no borders
    assert _platform_of(catalog, "8") == "comment"  # DateTimeBCD


def test_bit_mask_from_bit_position(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["3"]
    _platform, lines = gc.emit_entity(ev, "full")
    # Vitosoft numbers bits MSB-first inside the byte: BitPosition 2 -> 0x80>>2
    # = 0x20. Hardware-confirmed on 0x20CB via GWG_Flamme1~0x55DD (BitPosition
    # 2), which reads 0x20 exactly while the burner fires.
    assert any("bit_mask: 0x20" in ln for ln in lines)


def _bit_event(address: int, block_length: int, bit_position: int, byte_position: int = 0) -> "gc.Event":
    """A minimal read-only single-bit Event, as the access layer yields one."""
    return gc.Event(
        id=f"bit_{address:04X}_{bit_position}",
        name=f"Bit_{bit_position}",
        address=address,
        conversion="NoConversion",
        access_type=1,
        block_length=block_length,
        byte_length=1,
        byte_position=byte_position,
        bit_length=1,
        bit_position=bit_position,
        tech=f"Bit_{bit_position}",
        token=f"Bit_{bit_position}",
    )


@pytest.mark.parametrize(
    ("bit_position", "expected_mask"),
    [(0, "0x80"), (1, "0x40"), (2, "0x20"), (5, "0x04"), (7, "0x01")],
)
def test_bit_mask_is_msb_first(bit_position, expected_mask):
    platform, lines = gc.emit_entity(_bit_event(0x55DD, 1, bit_position), "full")
    assert platform == "binary_sensor"
    assert any(f"bit_mask: {expected_mask}" in ln for ln in lines)


def test_block_interior_bit_emits_aligned_block_read():
    # HK_Frostgefahr_aktivA1M1~0x2500: bit 135 of a 22-byte block = byte 16,
    # in-byte index 7 -> mask 0x01 (MSB-first). Previously rejected as
    # "exceeds binary_sensor length/offset limits"; binary_sensor now takes a
    # block read at the base plus byte_offset, exactly like sensor/text_sensor.
    platform, lines = gc.emit_entity(_bit_event(0x2500, 22, 135, byte_position=16), "full")
    assert platform == "binary_sensor"
    text = "\n".join(lines)
    assert "address: 0x2500" in text
    assert "length: 22" in text
    assert "byte_offset: 16" in text
    assert "bit_mask: 0x01" in text


def test_contradictory_byte_position_stays_a_comment():
    # BitPosition 24 -> byte 3, but the export declares BytePosition 2.
    # Real rows: nvoConsumerDmd_Attribute1_LFDM~0xA346, nviConsumerDmd_...~0xA385.
    platform, lines = gc.emit_entity(_bit_event(0xA346, 10, 24, byte_position=2), "full")
    assert platform == "comment"
    assert "contradictory" in "\n".join(lines)


def test_agreeing_byte_position_still_emits():
    platform, _lines = gc.emit_entity(_bit_event(0x2500, 22, 135, byte_position=16), "full")
    assert platform == "binary_sensor"


def test_bit_beyond_single_telegram_stays_a_comment():
    # A block wider than one P300 read telegram still cannot be expressed.
    platform, _lines = gc.emit_entity(_bit_event(0x1234, 64, 300), "full")
    assert platform == "comment"


def test_counter_gets_total_increasing_and_slow_poll(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["2"]
    _platform, lines = gc.emit_entity(ev, "full")
    text = "\n".join(lines)
    assert "state_class: total_increasing" in text
    assert f"update_interval: {gc.POLL_SLOW}s" in text


def test_writable_gets_coding_poll(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["6"]
    _platform, lines = gc.emit_entity(ev, "full")
    assert any(f"update_interval: {gc.POLL_CODING}s" in ln for ln in lines)


def test_sec2minute_emits_note(catalog):
    # No preset for Sec2Minute -> noconv + an explicit NOTE comment.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["9"]
    _platform, lines = gc.emit_entity(ev, "full")
    text = "\n".join(lines)
    assert "converter: noconv" in text
    assert "NOTE" in text and "Sec2Minute" in text


# --- profiles & filters -----------------------------------------------------


def test_full_profile_emits_more_than_minimal(catalog):
    full = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    minimal = gc.generate(catalog, "VTestHO1_99", "minimal", None, None)
    assert full.count("- platform: vitohome") >= minimal.count("- platform: vitohome")


def test_minimal_keeps_writables_and_measurements(catalog):
    minimal = gc.generate(catalog, "VTestHO1_99", "minimal", None, None)
    # div10 temp (measurement) and the writable select/number survive minimal.
    assert "0x0800" in minimal  # Outside_Temp
    assert "0x2301" in minimal  # Operating_Mode (writable)
    assert "0x6300" in minimal  # DHW_Setpoint (writable)


def test_include_filter(catalog):
    out = gc.generate(catalog, "VTestHO1_99", "full", r"Temp", None)
    assert "0x0800" in out  # Outside_Temp matches
    assert "0x2301" not in out  # Operating_Mode filtered out


def test_exclude_filter(catalog):
    out = gc.generate(catalog, "VTestHO1_99", "full", None, r"Temp")
    assert "0x0800" not in out
    assert "0x2301" in out


# --- emission shape ---------------------------------------------------------


def test_generate_has_platform_sections(catalog):
    out = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    for section in ("sensor:", "binary_sensor:", "number:", "select:", "text_sensor:"):
        assert section in out
    # Entities are opt-in.
    assert "disabled_by_default: true" in out
    # The hub-fed device identity is emitted by default.
    assert "type: device_id" in out
    # The 0x7507 DateTimeBCD slot is emitted as the error_history entity by
    # default (see test_error_history_entity_and_toggle), not as the older
    # "# Error Time @ 0x7507" DateTimeBCD comment hint.
    assert "type: error_history" in out
    assert "0x7507" in out
    assert "# Error Time @ 0x7507" not in out


def test_generated_yaml_parses(catalog):
    yaml = pytest.importorskip("yaml")

    out = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    # No !secret/!include tags in a generated package, so safe_load is fine.
    doc = yaml.safe_load(out)
    assert "sensor" in doc and "select" in doc


def test_error_history_entity_and_toggle(catalog):
    # The fixture's 0x7507 DateTimeBCD slot (Error_Time~0x7507) is recognised as
    # the error-history block. With emit_error_history on (the default) it is
    # emitted as the "Letzter Fehler" error_history text_sensor carrying the
    # openv code map; with it off, the slot falls back to the DateTimeBCD
    # comment hint. The address survives either way.
    on = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    assert "type: error_history" in on
    assert "Letzter Fehler" in on
    assert "# Error Time @ 0x7507" not in on
    assert "0x7507" in on

    off = gc.generate(catalog, "VTestHO1_99", "full", None, None, emit_error_history=False)
    assert "type: error_history" not in off
    assert "# Error Time @ 0x7507" in off
    assert "0x7507" in off


def test_reachable_filter_drops_non_optolink(catalog):
    # Event 10 (Unreachable_Temp~0x9999) is a normal Div10 sensor whose only
    # problem is FCRead=GFA_READ -- VitoWiFi's standard read can't reach it.
    on = gc.generate(catalog, "VTestHO1_99", "full", None, None)  # reachable_only defaults True
    assert "0x9999" not in on
    assert "Unreachable" not in on
    assert "FCRead filter ON: 1 datapoint" in on  # header reports exactly one dropped
    # Datapoints with a blank/unknown FCRead (e.g. 0x0800) are kept (benefit of doubt).
    assert "0x0800" in on
    # Opting out includes everything again, and the header note disappears.
    off = gc.generate(catalog, "VTestHO1_99", "full", None, None, reachable_only=False)
    assert "0x9999" in off
    assert "FCRead filter ON" not in off


# --- _friendly --------------------------------------------------------------


@pytest.mark.parametrize(
    "raw,expected",
    [
        # _friendly is deliberately light: strip a "Tech~0xADDR" tail and turn
        # '_' into spaces, but keep camelCase / coding-prefix compounds intact
        # and do NOT change case. A real translation is preferred where clean
        # labels matter; the stable technical id is carried as the entity id:.
        ("Outside_Temp~0x0800", "Outside Temp"),  # tail stripped, '_' -> space
        ("BurnerHours~0x08A7", "BurnerHours"),  # camelCase kept as one word
        ("status", "status"),  # passed through verbatim
        ("Aussentemperatur", "Aussentemperatur"),  # non-@@ label passes through
        ("@@x.y.name.K00_Konfi", "K00 Konfi"),  # unresolved @@ id -> readable
    ],
)
def test_friendly(raw, expected):
    assert gc._friendly(raw) == expected


def test_friendly_resolves_translation():
    # An @@-prefixed label resolves verbatim against the text map (incl. non-ASCII).
    textmap = {"v.eventtype.name.X": "Übersetzt"}
    assert gc._friendly("@@v.eventtype.name.X", textmap) == "Übersetzt"


def test_unknown_device_raises(catalog):
    with pytest.raises(SystemExit):
        gc.generate(catalog, "DoesNotExist", "full", None, None)


def test_signed_emitted_for_negative_bound_noconv_number(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    # Event 11 (Frost_Limit, noconv, lower border -9) must carry signed: true.
    _, neg = gc.emit_entity(ev["11"], "full")
    assert "  converter: noconv" in neg
    assert "  signed: true" in neg
    assert "  min_value: -9" in neg
    # Event 6 (DHW_Setpoint, noconv, lower border +10) must NOT be signed.
    _, pos = gc.emit_entity(ev["6"], "full")
    assert "  converter: noconv" in pos
    assert "  signed: true" not in pos


def test_non_virtual_write_demoted_to_readonly(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    # Event 12: Type 3 (writable) but FCWrite=undefined -> read-only enum text_sensor.
    plat12, lines12 = gc.emit_entity(ev["12"], "full")
    assert plat12 == "text_sensor"
    assert "type: enum" in "\n".join(lines12)
    # Event 5: genuinely writable enum (no FCWrite override) stays a select.
    plat5, _ = gc.emit_entity(ev["5"], "full")
    assert plat5 == "select"


def test_hexbyte2ascii_emitted_as_ascii_text_sensor(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    # Event 13: HexByte2AsciiByte, 7 bytes -> ascii text_sensor (not a comment).
    plat, lines = gc.emit_entity(ev["13"], "full")
    assert plat == "text_sensor"
    body = "\n".join(lines)
    assert "type: ascii" in body
    assert "length: 7" in body


def test_command_state_split_emits_state_address(catalog):
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}
    # Event 14: party command at 0x2330. Its EIN/AUS pair makes it a *switch*
    # since the boolean-pair heuristic landed (a two-option select before);
    # the COMMAND_STATE_ADDR split must survive on the switch emission path.
    plat, lines = gc.emit_entity(ev["14"], "full")
    assert plat == "switch"
    body = "\n".join(lines)
    assert "address: 0x2330" in body  # command (write) address
    assert "state_address: 0x2303" in body  # live-state (read) address
    assert "on 0x01" in body and "off 0x00" in body  # default 1/0 documented


# --- bulk export (--export-all) --------------------------------------------


def test_export_stem_matches_viessmann_token():
    # The Viessmann token already is the unit_swIndex[_variant] key; we only
    # lower-case it. VScotHO1_72 -> vscotho1_72 (Tom's family), as specified.
    assert gc._export_stem("VScotHO1_72") == "vscotho1_72"
    assert gc._export_stem("GWG_VBES_00") == "gwg_vbes_00"
    assert gc._export_stem("VScotHO1_200_10") == "vscotho1_200_10"


def test_export_stem_sanitises_unsafe_chars():
    # Anything outside [a-z0-9._-] collapses to '_'; leading/trailing junk trimmed.
    assert gc._export_stem("Dev/One") == "dev_one"
    assert gc._export_stem("  A B:C  ") == "a_b_c"
    assert gc._export_stem("///") == "device"  # never empty


def test_identity_fields_boiler_2byte_extension():
    # Real VScotHO1_72 identification row shape: ident 0x20CB, ext 0x0148 (HW high
    # byte 0x01, SW low byte 0x48 = 72), till 0x0159 (SW 0x59 = 89).
    row = {
        "ident": 0x20CB,
        "ext": 0x0148,
        "extt": 0x0159,
        "f0": None,
        "f0t": None,
        "IdentificationExtension": "0148",
    }
    f = gc._identity_fields(row)
    assert f["ident"] == "0x20CB"
    assert f["hw_index"] == 1
    assert f["sw_lo"] == 72
    assert f["sw_hi"] == 89
    assert f["f0_lo"] == "" and f["f0_hi"] == ""


def test_identity_fields_serial_extension_left_blank():
    # M-Bus meters carry a 6-byte serial in the extension, not HW<<8|SW; the
    # numeric columns must stay blank and the raw string is preserved.
    row = {
        "ident": 0x0611,
        "ext": 0x343230343038,  # > 0xFFFF -> not a 2-byte HW/SW extension
        "extt": None,
        "f0": None,
        "f0t": None,
        "IdentificationExtension": "343230343038",
    }
    f = gc._identity_fields(row)
    assert f["ident"] == "0x0611"
    assert f["hw_index"] == "" and f["sw_lo"] == "" and f["sw_hi"] == ""
    assert f["ext_raw"] == "343230343038"


def test_identity_fields_f0_range():
    row = {
        "ident": 0x20CB,
        "ext": 0x01C8,
        "extt": 0x01FF,
        "f0": 20,
        "f0t": 29,
        "IdentificationExtension": "01C8",
    }
    f = gc._identity_fields(row)
    assert (f["sw_lo"], f["sw_hi"]) == (200, 255)
    assert (f["f0_lo"], f["f0_hi"]) == (20, 29)


def test_identity_fields_empty_row_is_all_blank():
    f = gc._identity_fields({})
    assert f["ident"] == "" and f["hw_index"] == "" and f["ext_raw"] == ""


def _read_index(out_dir):
    import csv

    with open(os.path.join(out_dir, "index.csv"), encoding="utf-8", newline="") as fh:
        return {r["token"]: r for r in csv.DictReader(fh)}


def test_export_all_writes_per_device_and_skips_empty(catalog, tmp_path):
    rc = gc.export_all(
        catalog,
        str(tmp_path),
        profile="standard",
        include_re=None,
        exclude_re=None,
        token_filter=None,
        suffix=".yaml",
        emit_device_id=True,
        emit_error_history=True,
        error_codes=False,
        error_code_set="vd300",
        reachable_only=True,
    )
    assert rc == 0
    # VTestHO1_99 has 19 linked events -> written; VOther_01 has none -> skipped.
    assert (tmp_path / "vtestho1_99.yaml").is_file()
    assert not (tmp_path / "vother_01.yaml").exists()
    idx = _read_index(str(tmp_path))
    assert idx["VTestHO1_99"]["file"] == "vtestho1_99.yaml"
    assert idx["VTestHO1_99"]["status"] == "ok"
    assert idx["VTestHO1_99"]["events"] == "20"  # datapoints LINKED
    # entities EMITTED is a separate, smaller count (comments + write-only are
    # not entities); it is a positive integer and never exceeds the link count.
    assert idx["VTestHO1_99"]["entities"].isdigit()
    assert 0 < int(idx["VTestHO1_99"]["entities"]) <= 40  # Schaltzeiten fans 1->7
    assert idx["VOther_01"]["status"] == "skipped: no events"
    assert idx["VOther_01"]["file"] == ""


def test_export_all_filter_selects_subset(catalog, tmp_path):
    rc = gc.export_all(
        catalog,
        str(tmp_path),
        profile="standard",
        include_re=None,
        exclude_re=None,
        token_filter="^VOther",
        suffix=".yaml",
        emit_device_id=True,
        emit_error_history=True,
        error_codes=False,
        error_code_set="vd300",
        reachable_only=True,
    )
    # Only VOther_01 matches, and it has no events -> nothing written -> rc 1.
    assert rc == 1
    idx = _read_index(str(tmp_path))
    assert set(idx) == {"VOther_01"}


def test_export_all_custom_suffix(catalog, tmp_path):
    gc.export_all(
        catalog,
        str(tmp_path),
        profile="standard",
        include_re=None,
        exclude_re=None,
        token_filter="^VTestHO1",
        suffix=".dp.yaml",
        emit_device_id=True,
        emit_error_history=True,
        error_codes=False,
        error_code_set="vd300",
        reachable_only=True,
    )
    assert (tmp_path / "vtestho1_99.dp.yaml").is_file()


def test_export_all_via_main_cli(tmp_path):
    out = tmp_path / "catalogs"
    rc = gc.main(["--data", _FIXTURE_DIR, "--export-all", "--out", str(out), "--no-error-codes"])
    assert rc == 0
    assert (out / "vtestho1_99.yaml").is_file()
    assert (out / "index.csv").is_file()


def test_export_all_requires_out(capsys):
    # argparse .error() raises SystemExit(2); --export-all without --out must fail.
    with pytest.raises(SystemExit):
        gc.main(["--data", _FIXTURE_DIR, "--export-all"])


# --- audit fixes: write-only, converter-width, negative enum, factor, signed --


def test_write_only_register_is_a_comment_not_a_number(catalog):
    # Event 15 (Maintenance_Reset, Vitosoft Type 2 = AccessMode 'Write'): a
    # trigger with no read-back must be surfaced as a comment hint, never a
    # polled number/switch.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["15"]
    plat, lines = gc.emit_entity(ev, "full")
    assert plat == "comment"
    body = "\n".join(lines)
    assert "WRITE-ONLY" in body
    assert "0x5724" in body


def test_unsupported_converter_width_falls_back_to_noconv_filter(catalog):
    # Event 16 (Div10 at 4 bytes): the component's div10 only decodes 1-2
    # bytes, so a `converter: div10, length: 4` would fail `esphome config`.
    # It must fall back to raw noconv + a multiply(0.1) filter carrying the
    # true scale, and keep div10's signed default via an explicit signed: true.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["16"]
    plat, lines = gc.emit_entity(ev, "full")
    assert plat == "sensor"
    body = "\n".join(lines)
    assert "converter: noconv" in body
    assert "converter: div10" not in body
    assert "signed: true" in body
    assert "- multiply: 0.1" in body
    assert "NOTE" in body and "unsupported at 4 bytes" in body


def test_negative_enum_option_dropped_with_note(catalog):
    # Event 17: an enum option with EnumAddressValue -1 can never match on the
    # wire (raw bytes compare unsigned) and emitting it produced an invalid
    # 0x-1 key. The -1 option is dropped, a NOTE names the drop, and the 0/1
    # options survive with valid keys.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["17"]
    plat, lines = gc.emit_entity(ev, "full")
    assert plat == "text_sensor"
    body = "\n".join(lines)
    assert "0x-1" not in body
    assert "0x00:" in body and "0x01:" in body
    assert "NOTE" in body and "negative enum option" in body
    # And exactly one option was dropped.
    assert gc._negative_option_count(ev) == 1


def test_multoffset_factor_maps_to_preset(catalog):
    # Event 18 (MultOffset, ConversionFactor 10, offset 0): value = raw * 10
    # maps exactly onto the mult10 converter. Previously emitted as raw noconv
    # (a 10x under-read); must now be converter: mult10 with no filter.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["18"]
    plat, lines = gc.emit_entity(ev, "full")
    assert plat == "sensor"
    body = "\n".join(lines)
    assert "converter: mult10" in body
    assert "filters:" not in body
    assert "NOTE" in body and "mult10" in body


def test_signed_inferred_for_readonly_noconv_negative_border(catalog):
    # Event 19 (read-only noconv, LowerBorder -40): noconv is unsigned by
    # default, so a value range crossing zero must force signed: true on the
    # sensor -- otherwise -1.0 would decode as +65535.
    ev = {e.id: e for e in catalog.events_for("VTestHO1_99")}["19"]
    plat, lines = gc.emit_entity(ev, "full")
    assert plat == "sensor"
    body = "\n".join(lines)
    assert "converter: noconv" in body
    assert "signed: true" in body


def test_block_factor_drives_error_history_slot_count():
    # A system error archive whose access layer declares BlockLength 45 /
    # BlockFactor 5 must expand into exactly 5 nine-byte slots at base + i*9,
    # not the legacy 10. (Direct Event construction: the fixture's own archive
    # exercises the >=90 fallback; this pins the BlockFactor path.)
    ev = gc.Event(
        id="900",
        name="ecnsysEventType~Error",
        address=0x7507,
        conversion="",
        access_type=1,
        block_length=45,
        byte_length=45,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        block_factor=5,
        tech="ecnsysEventType~Error",
        token="ecnsysEventType~Error",
    )
    entries = gc._error_history_entries(ev)
    assert len(entries) == 5
    assert entries[0]["address"] == 0x7507 and entries[0]["name"] == "Letzter Fehler"
    assert entries[4]["address"] == 0x7507 + 36
    assert all(e["system"] is True for e in entries)


def test_order_group_inserts_section_comments(catalog):
    # --order group groups entities by the Vitosoft navigation tree and inserts
    # a `# --- <label> ---` comment per group. The fixture links event 1
    # (Outside_Temp) and event 5 (Operating_Mode) under Bedienung, event 6
    # (DHW_Setpoint) under Warmwasser.
    grouped = gc.generate(catalog, "VTestHO1_99", "full", None, None, order="group")
    assert "# --- Bedienung" in grouped
    assert "# --- Warmwasser" in grouped
    assert "(ohne Gruppenzuordnung)" in grouped  # ungrouped datapoints land here
    # Address order (the default) emits no per-group section comments (the
    # trailing "custom decode" header is not a group marker).
    by_addr = gc.generate(catalog, "VTestHO1_99", "full", None, None, order="address")
    assert "# --- Bedienung" not in by_addr
    assert "# --- Warmwasser" not in by_addr


def test_stats_out_param_reports_counts(catalog):
    # generate() still returns a str; the optional stats dict receives the
    # emitted-entity and comment counts so export_all can spot empty shells.
    stats: dict = {}
    text = gc.generate(catalog, "VTestHO1_99", "full", None, None, stats=stats)
    assert isinstance(text, str)
    assert stats["entities"] > 0
    assert stats["comments"] >= 1  # event 15 (write-only) and event 8 (BCD) are comments


# --- audit fixes round 2: bugs caught by `esphome config` on the bulk export --


def test_conversion_factor_zero_is_not_a_multiplier():
    # ConversionFactor 0 is the export's dominant "no scaling" sentinel (4364
    # NoConversion rows), NOT a literal 0x multiplier. A read-only noconv row
    # carrying factor 0 must decode raw (no filter), never `multiply: 0` which
    # would zero every reading.
    ev = gc.Event(
        id="700",
        name="Zeroed",
        address=0x0A31,
        conversion="NoConversion",
        access_type=1,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        conv_factor=0.0,  # the sentinel
        conv_offset=0.0,
        tech="zeroed",
        token="Zeroed~0x0A31",
    )
    plat, lines = gc.emit_entity(ev, "full")
    assert plat == "sensor"
    body = "\n".join(lines)
    assert "converter: noconv" in body
    assert "multiply" not in body
    assert "filters:" not in body


def test_error_history_slot_stride_follows_block_factor():
    # A 120-byte / BlockFactor-10 system archive has 12-byte records, so slot
    # addresses must advance by 12 (BlockLength // BlockFactor), not a
    # hardcoded 9. (The component still reads the leading 9 bytes at each slot.)
    ev = gc.Event(
        id="710",
        name="ecnsysEventType~VitotwinErrorHistorySW02",
        address=0x7000,
        conversion="",
        access_type=1,
        block_length=120,
        byte_length=120,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        block_factor=10,
        tech="ecnsysEventType",
        token="ecnsysEventType~VitotwinErrorHistorySW02",
    )
    entries = gc._error_history_entries(ev)
    assert len(entries) == 10
    assert entries[1]["address"] - entries[0]["address"] == 12
    assert entries[9]["address"] == 0x7000 + 9 * 12


def test_second_system_archive_names_do_not_collide():
    # A unit with two system archives (canonical ~Error plus a distinct
    # ~Vitotwin... archive) must not emit two entities both named
    # "Letzter Fehler" -- ESPHome rejects duplicate names per platform. The
    # canonical archive stays plain; the secondary one gets a tag.
    canonical = gc.Event(
        id="720",
        name="ecnsysEventType~Error",
        address=0x7507,
        conversion="",
        access_type=1,
        block_length=90,
        byte_length=90,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        block_factor=10,
        tech="ecnsysEventType",
        token="ecnsysEventType~Error",
    )
    secondary = gc.Event(
        id="721",
        name="ecnsysEventType~VitotwinErrorHistorySW02",
        address=0x7000,
        conversion="",
        access_type=1,
        block_length=120,
        byte_length=120,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        block_factor=10,
        tech="ecnsysEventType",
        token="ecnsysEventType~VitotwinErrorHistorySW02",
    )
    c_names = {e["name"] for e in gc._error_history_entries(canonical)}
    s_names = {e["name"] for e in gc._error_history_entries(secondary)}
    assert "Letzter Fehler" in c_names  # canonical stays plain
    assert "Letzter Fehler" not in s_names  # secondary is tagged
    assert c_names.isdisjoint(s_names)  # no overlap at all


def test_duplicate_enum_value_is_deduplicated():
    # The export can map two options to the SAME value (LON/BACnet nodes);
    # duplicate values would become duplicate YAML option keys that fail
    # `esphome config`. _enum_options keeps the first, drops the rest.
    ev = gc.Event(
        id="730",
        name="Alarm_Type",
        address=0x082E,
        conversion="NoConversion",
        access_type=1,
        block_length=1,
        byte_length=1,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        enum_type=True,
        tech="alarm_type",
        token="Alarm_Type~0x082E",
        values=[
            gc.EventValue(
                name="a",
                enum_address_value=3,
                enum_replace_value="Service alarm 2",
                description="",
                unit="",
                lower="",
                upper="",
                stepping="",
            ),
            gc.EventValue(
                name="b",
                enum_address_value=3,
                enum_replace_value="Service alarm 3",
                description="",
                unit="",
                lower="",
                upper="",
                stepping="",
            ),
            gc.EventValue(
                name="c",
                enum_address_value=4,
                enum_replace_value="Other",
                description="",
                unit="",
                lower="",
                upper="",
                stepping="",
            ),
        ],
    )
    opts = gc._enum_options(ev)
    values = [v for v, _ in opts]
    assert values == [3, 4]  # the second value-3 row dropped, order preserved
    assert opts[0][1] == "Service alarm 2"  # first-wins


def test_multiple_gfa_style_archives_get_distinct_family_names():
    # Vitovalor carries FehlerHisFA01.. (GFA), Fehlerhistorie_FCU_N and
    # Fehlerhist_FCU_N (two fuel-cell archives); all match the FehlerHis slot
    # regex and previously all named their slots "GFA Fehler NN" -> a duplicate
    # name that fails `esphome config`. Each family must now be distinct.
    def _fa(token, addr):
        return gc.Event(
            id=token,
            name=token,
            address=addr,
            conversion="",
            access_type=1,
            block_length=9,
            byte_length=9,
            byte_position=0,
            bit_length=0,
            bit_position=0,
            tech=token,
            token=token,
        )

    gfa = gc._error_history_entries(_fa("FehlerHisFA01~0x7590", 0x7590))[0]
    fcu_a = gc._error_history_entries(_fa("Fehlerhistorie_FCU_1~0xD709", 0xD709))[0]
    fcu_b = gc._error_history_entries(_fa("Fehlerhist_FCU_1~0xD763", 0xD763))[0]
    assert gfa["name"] == "GFA Fehler 01"  # canonical family stays plain
    names = {gfa["name"], fcu_a["name"], fcu_b["name"]}
    assert len(names) == 3  # all three families distinct
    seeds = {gfa["seed"], fcu_a["seed"], fcu_b["seed"]}
    assert len(seeds) == 3


# --- Schaltzeiten: 56-byte weekday programs -> 7 per-day text entities --------


def _schaltzeiten_event(token, addr, blen, bf):
    return gc.Event(
        id=token,
        name=token,
        address=addr,
        conversion="NoConversion",
        access_type=3,
        block_length=blen,
        byte_length=blen,
        byte_position=0,
        bit_length=0,
        bit_position=0,
        block_factor=bf,
        tech=token,
        token=f"{token}~0x{addr:04X}",
    )


def test_schaltzeiten_standard_shape_expands_to_seven_text_entities():
    # A 56-byte / BlockFactor-7 program (8-byte records) is the shape the
    # component's `text` platform decodes: expand to 7 per-day entities at
    # base + day*8, Monday first.
    ev = _schaltzeiten_event("Schaltzeiten_A1M1_HK", 0x2000, 56, 7)
    assert gc._is_schaltzeiten(ev) is True
    entries = gc._schaltzeiten_entries(ev)
    assert len(entries) == 7
    assert entries[0]["address"] == 0x2000
    assert entries[0]["name"] == "Schaltzeit A1M1 HK Montag"
    assert entries[6]["address"] == 0x2000 + 6 * 8
    assert entries[6]["name"] == "Schaltzeit A1M1 HK Sonntag"
    # seeds are unique per day
    assert len({e["seed"] for e in entries}) == 7


@pytest.mark.parametrize(
    "token,addr,blen,bf",
    [
        ("WPR3_Schaltzeit_Kuehlpuffer", 0x93F8, 168, 56),  # heat-pump: 3-byte records
        ("HO2B_Lueftung_Schaltzeiten", 0xCA00, 168, 7),  # ventilation: 24-byte records
        ("WPR_vmarSchaltzeitenGroup_1_0", 0x5000, 24, 0),  # LON group: no BlockFactor
        ("KBUS_HV_Schaltzeit_B1_Dienstag_Aus_9", 0x6000, 1, 0),  # pre-decomposed field
    ],
)
def test_schaltzeiten_nonstandard_shapes_are_rejected(token, addr, blen, bf):
    # Only the 8-byte-record shape is decodable by the component; every other
    # Schaltzeiten shape must fall through to the generic path so it is NOT
    # mis-emitted as a weekday text entity.
    ev = _schaltzeiten_event(token, addr, blen, bf)
    assert gc._is_schaltzeiten(ev) is False


def test_schaltzeiten_text_entity_yaml_is_valid():
    ev = _schaltzeiten_event("Schaltzeiten_M2_WW", 0x3100, 56, 7)
    entry = gc._schaltzeiten_entries(ev)[2]  # Wednesday
    lines = gc._schaltzeiten_lines(entry, "schaltzeit_m2_ww_mi")
    body = "\n".join(lines)
    assert "platform: vitohome" in body
    assert "address: 0x3110" in body  # 0x3100 + 2*8
    assert "disabled_by_default: true" in body
    # text entities carry no converter/length (the platform hardcodes both)
    assert "converter:" not in body
    assert "length:" not in body


def test_generate_emits_schaltzeiten_in_text_section():
    # End-to-end: the fixture carries a 56/7 Schaltzeiten event; generate()
    # must place its 7 days under a `text:` section, not in the custom-decode
    # comment block.
    import os

    fixture_dir = os.path.join(os.path.dirname(__file__), "fixtures")
    cat = gc.load_catalog(fixture_dir)
    text = gc.generate(cat, "VTestHO1_99", "full", None, None)
    assert "\ntext:\n" in text
    assert "Schaltzeit Test Montag" in text
    assert text.count("Schaltzeit Test ") == 7  # seven weekday entities
    assert "custom decode" not in text.split("\ntext:\n")[1].split("\ntext_sensor:")[0]
