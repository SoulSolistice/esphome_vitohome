#!/usr/bin/env python3
"""Generate a vitohome ESPHome package from a Viessmann Vitosoft XML export.

Per-device datapoint definitions (address, length, conversion, access type,
enums, units, borders) are NOT discoverable over the Optolink bus -- they come
from Viessmann's own Vitosoft data, surfaced by the InsideViessmannVitosoft
scripts. This script reads that export and emits a ready-to-include ESPHome
*package* with each datapoint on the right platform (sensor / binary_sensor /
number / select / switch / text / text_sensor), the correct ``converter:``,
real ``length:``, bit masks, units and a sensible poll interval.

The export splits the data across several files; the generator reads all of
them:

  DPDefinitions.xml      .NET DataSet diffgram. Tables (repeated elements):
                         ecnDatapointType (device types; Address = token),
                         ecnDataPointTypeEventTypeLink (DataPointTypeId ->
                         EventTypeId), ecnEventType (Name, Address-*token*,
                         Conversion, Type, EnumType), ecnEventTypeEventValueType
                         Link (EventTypeId -> *EventValueId*), ecnEventValueType
                         (enum values + Unit + borders), ecnEventTypeGroup and
                         ecnEventTypeEventTypeGroupLink (the menu tree).
                         The diffgram declares
                         xmlns="http://tempuri.org/ECNDataSet.xsd", so every
                         element is namespaced -- tags are matched by LOCAL name.
  ecnEventType.xml       ACCESS layer: per event token (its <ID>), the real
                         Optolink <Address> 0xNNNN, BlockLength/ByteLength,
                         BitLength/BitPosition/BytePosition, FCRead/FCWrite,
                         ConversionFactor, Unit. DPDefinitions carries no
                         lengths; they live here and are joined on the event's
                         Address token.
  ecnDataPointType.xml   IDENTIFICATION: per datapoint-type token, Identification
                         (group 0xF8 << 8 | ident 0xF9) and IdentificationExt..
                         Till (HW 0xFA << 8 | SW 0xFB). Used by --identify to
                         pick the correct software revision automatically.
  Textresource*.xml      UTF-16; <TextResource Label=.. Value=.. CultureId=..>.
                         Resolves @@-prefixed names, enum labels and fault texts.
                         The slim Textresource.xml carries UI strings only; the
                         full Textresource_<culture>.xml files carry every name.

Typical use::

    # what device tables (and identification ranges) does this export contain?
    python3 scripts/gen_catalog.py --data <export-dir> --list-devices

    # pick the revision automatically from the bytes the hub already logs:
    #   Device: 0x20CB (VScotHO1) HW=0x03 SW=0x51
    python3 scripts/gen_catalog.py --data <export-dir> \\
        --identify 0x20CB --hw 0x03 --sw 0x51 \\
        --profile standard --out my-heater.vitohome.yaml

    # ...or name the datapoint-type token directly
    python3 scripts/gen_catalog.py --data <export-dir> --device VScotHO1_72 ...

    # generate a catalog for EVERY device token at once (one file per unit),
    # named unit_swIndex[_variant].yaml (vscotho1_72.yaml, gwg_vbes_00.yaml, ...)
    # with an index.csv manifest; narrow with --export-filter if wanted:
    python3 scripts/gen_catalog.py --data <export-dir> --export-all --out ./catalogs
    python3 scripts/gen_catalog.py --data <export-dir> --export-all --out ./catalogs \\
        --export-filter '^V' --no-error-codes

GWG devices are handled with a different rule set, selected automatically from
the Vitosoft device token (``GWG_*`` / ``HV_GWG``; ``--protocol`` overrides)::

    python3 scripts/gen_catalog.py --data <export-dir> --device GWG_VBES_00 \\
        --profile standard --out gwg.yaml

GWG carries the access mode in the telegram TYPE byte and its address space is
PER MODE -- in ``GWG_VBES_00``, address 0x01 is five different datapoints across
four modes. The generator therefore maps each event's ``FCRead`` onto the
component's ``access:`` option (see FCREAD_TO_GWG_ACCESS) and emits it on every
entity; the address alone would be ambiguous. It also inverts the reachability
rule (on P300 only ``Virtual_READ`` is readable; on GWG that is a small share of
the data and the rest is readable on its own mode), maps ``FCWrite`` the same
way so writable datapoints become number/select/switch in the mode they also
read in (see FCWRITE_TO_GWG_ACCESS; the KMBUS modes have no write TYPE byte and
stay read-only), and drops addresses above 0xFF.

Include the emitted file from your device YAML::

    packages:
      heater: !include my-heater.vitohome.yaml

and define the ``vitohome:`` hub (and ``uart:``) yourself. Every entity is
``disabled_by_default: true`` so you opt in from Home Assistant. Always run
``esphome config`` then ``esphome compile``/``run`` before relying on a value --
not every address answers on every firmware.

The implementation lives in the ``vitocatalog`` package next to this script;
this file is the CLI entry point and re-exports the package's names for
existing callers. stdlib only; runs in CI and in a bare Python install.
"""

from __future__ import annotations

import argparse
import os
import sys

# Ensure sibling modules (fault_codes, units, vitocatalog) are importable when
# invoked from any working directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fault_codes  # noqa: E402, F401
import units  # noqa: E402, F401
from vitocatalog import (  # noqa: E402, F401
    _BOOLEAN_ON_OFF_PAIRS,
    _CONTAINER_PREFIXES,
    _CONV_LENGTHS,
    _CONV_SCALE,
    _CULTURES,
    _ERROR_HISTORY_ADDRESS,
    _FACTOR_PLACEHOLDERS,
    _FACTOR_PRESETS,
    _IDENT_ADDRESSES,
    _MANIFEST_COLUMNS,
    _SCHALTZEITEN_BLOCK_LEN,
    _SCHALTZEITEN_DAYS,
    _SCHALTZEITEN_RECORD_LEN,
    _STEP_TOL,
    _UI_TEXT,
    _UNSIGNED_PARAMETER_TYPES,
    _WANTED_ROWS,
    _WEEKDAYS,
    ACCESS_PLATFORMS,
    ACTION_WARNING,
    COMMAND_STATE_ADDR,
    CONVERSION_MAP,
    COUNTER,
    DIV,
    F0_ADDRESSES,
    FCREAD_TO_GWG_ACCESS,
    FCWRITE_TO_GWG_ACCESS,
    GFA_DIRNAME,
    MAX_P300_READ_LENGTH,
    MAX_RMW_BLOCK,
    MAX_TEXT_BLOCK_LENGTH,
    NON_NUMERIC_CONVERSIONS,
    PLAIN,
    POLL_CODING,
    POLL_ERROR,
    POLL_FAST,
    POLL_SLOW,
    PROFILES,
    PROTO_GWG,
    PROTO_KW,
    PROTO_P300,
    RESOLVE_FILENAME,
    RESOLVE_SCHEMA,
    RPC_PLATFORMS,
    RPC_PREFIX_MAX,
    BoundsScale,
    Catalog,
    CommentSpec,
    EntitySpec,
    Event,
    EventValue,
    _access_for,
    _address_from_name,
    _address_from_token,
    _bit_field,
    _block_write_blocker,
    _boolean_pair,
    _bound_fits,
    _build_bit_field_spec,
    _child,
    _clean_name,
    _codes_lines,
    _declared_unsigned,
    _dedup_option_labels,
    _device_class_for,
    _device_id_lines,
    _dump_resolve_index,
    _emit_bit_field,
    _enum_bool_pair,
    _enum_options,
    _enum_read_length,
    _error_history_archive_tag,
    _error_history_entries,
    _error_history_lines,
    _error_history_slot,
    _esphome_object_id,
    _export_stem,
    _find_dpdefinitions,
    _find_file,
    _friendly,
    _hexaddr,
    _hx,
    _identity_fields,
    _inject_diagnostic,
    _intval,
    _is_diagnostic,
    _is_error_history,
    _is_hw_sw_ext,
    _is_identification,
    _is_lon_bound_rpc,
    _is_lon_input,
    _is_mode_select,
    _is_reachable,
    _is_rpc_action,
    _is_rpc_reading,
    _is_schaltzeiten,
    _is_writable,
    _llround,
    _load_access,
    _load_identification,
    _load_text_layers,
    _load_textresources,
    _local,
    _lookup_label,
    _make_names_unique,
    _make_obj_id,
    _mode_select_caveat,
    _negative_option_count,
    _norm_bool_label,
    _overlay,
    _poll_for,
    _profile_keep,
    _protocol_for_device,
    _read_only_blocks,
    _read_text,
    _readable,
    _resolve_fault_codes,
    _resolve_gfa_codes,
    _resolve_label,
    _resolve_run_key,
    _resolve_runs,
    _resolve_unit,
    _rpc_array_records,
    _rpc_prefix,
    _scale_from_bounds,
    _schaltzeiten_entries,
    _schaltzeiten_lines,
    _schaltzeiten_program_label,
    _share_codes_blocks,
    _string_lines,
    _string_spec,
    _strip_default_ns,
    _tech_id,
    _ui,
    _unit_for,
    _unit_negated,
    _whole_steps,
    _yaml_str,
    build_entity_spec,
    build_resolve_index,
    emit_entity,
    export_all,
    f0_bytes,
    generate,
    gfa_chip_names,
    gfa_companion,
    gfa_companion_name,
    load_catalog,
    render_entity_lines,
)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "--data", required=True, help="dir with the Vitosoft XML export (DPDefinitions*.xml, ecnEventType.xml, ...)"
    )
    p.add_argument("--device", help="datapoint-type token to generate for, e.g. VScotHO1_72")
    p.add_argument("--identify", help="device Identification hex (group<<8|ident), e.g. 0x20CB; auto-selects the revision")
    p.add_argument("--hw", help="hardware index hex (0xFA), used with --identify")
    p.add_argument("--sw", help="software index hex (0xFB), used with --identify")
    p.add_argument(
        "--f0",
        help="F0, the value at 0x00F0 (hex), optional, used with --identify: one byte, or the "
        "two raw bytes the hub logs as F0=0xXXYY",
    )
    p.add_argument("--list-devices", action="store_true", help="list device tokens (+ identification ranges) and exit")
    p.add_argument(
        "--profile",
        choices=PROFILES,
        default="standard",
        help="how many datapoints to emit: full = everything Vitosoft lists; standard = without LON "
        "network-variable inputs (nvi*), which only a LON master fills; minimal = writable values and "
        "scaled measurements (default: standard)",
    )
    p.add_argument("--include", help="regex; only emit events whose name matches")
    p.add_argument("--exclude", help="regex; drop events whose name matches")
    p.add_argument(
        "--culture",
        default="de",
        choices=sorted(_CULTURES),
        help="language for entity names, option labels and fault texts, from "
        "Textresource_<culture>.xml in --data (German per label where a text is missing). "
        "Units never change with the language (units.py). Default: de",
    )
    p.add_argument(
        "--menu-datapoints",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="also take the datapoints a device carries only in its own Vitosoft menu tree, not in its "
        "direct datapoint list (150 devices, up to 161 datapoints each) (default: on)",
    )
    p.add_argument(
        "--device-id",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="emit a device_id diagnostic text_sensor and suppress the raw 0xF8..0xFB reads (default: on)",
    )
    p.add_argument(
        "--partial-writes",
        "--bitfield-writes",
        dest="partial_writes",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "emit writable fields that are smaller than their datapoint -- bit fields (bit_mask) and "
            "fields inside a block (byte_offset) -- which the component writes as a read-modify-write "
            "of the whole byte or block. --no-partial-writes keeps them read-only. "
            "--[no-]bitfield-writes is the older spelling (default: on)"
        ),
    )
    p.add_argument(
        "--error-history",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="emit error_history text_sensors for the FehlerHis* slots (or 0x7507 fallback) (default: on)",
    )
    p.add_argument(
        "--error-codes",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="attach a fault-code map (see --error-code-set) to error_history entities (default: on)",
    )
    p.add_argument(
        "--error-code-set",
        choices=["auto", "vitosoft", *sorted(fault_codes.SETS)],
        default="auto",
        help="fault-code texts for the system error history. auto (default): Vitosoft's own texts "
        "for this device from Textresource_<culture>.xml, else its generic Vitotronic set "
        "(ErrorType 1 devices only), else none; vd300 when the export has no fault texts at all. "
        "vitosoft: the same without the vd300 fallback. vd300/vd200/openv/union: the "
        "hand-curated maps in fault_codes.py",
    )
    p.add_argument(
        "--gfa-chip",
        help="hex byte read from GFA_Kennung (0x7650), e.g. 24: attach Vitosoft's texts for that "
        "burner-control chip to the GFA error-history slots (single device only)",
    )
    p.add_argument(
        "--protocol",
        choices=[PROTO_P300, PROTO_KW, PROTO_GWG],
        default=None,
        help="Optolink protocol the catalog targets. Default: inferred per device token "
        "(a Vitosoft GWG_* / *_GWG token means GWG, everything else P300), which is what lets "
        "--export-all emit both families in one run.",
    )
    p.add_argument(
        "--reachable-only",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="emit only datapoints VitoWiFi can read over Optolink (FCRead Virtual_READ); "
        "drop GFA_READ/RPC/PROZESS/KBUS/OT (default: on)",
    )
    p.add_argument(
        "--export-all",
        action="store_true",
        help="generate one catalog per device token into --out (a directory)",
    )
    p.add_argument(
        "--export-filter",
        help="with --export-all: regex; only export tokens whose ID matches",
    )
    p.add_argument(
        "--export-suffix",
        default=".yaml",
        help="with --export-all: file-name extension for each catalog (default: .yaml)",
    )
    p.add_argument(
        "--order",
        choices=("address", "group"),
        default=None,
        help="entity ordering: 'address' or 'group'. Default: 'group' with --export-all, 'address' otherwise",
    )
    p.add_argument("--out", help="output file (default: stdout); with --export-all, the output DIRECTORY")
    args = p.parse_args(argv)

    # --order defaults per mode (see the flag's help): the committed
    # example/catalogs come from --export-all and are group-ordered, so a
    # re-export keeps their order. An explicit --order wins in both modes.
    if args.order is None:
        args.order = "group" if args.export_all else "address"

    catalog = load_catalog(args.data, culture=args.culture)
    catalog.menu_datapoints = args.menu_datapoints

    if args.export_all:
        if not args.out:
            p.error("--export-all requires --out <directory>")
        return export_all(
            catalog,
            args.out,
            profile=args.profile,
            include_re=args.include,
            exclude_re=args.exclude,
            token_filter=args.export_filter,
            suffix=args.export_suffix,
            emit_device_id=args.device_id,
            emit_error_history=args.error_history,
            error_codes=args.error_codes,
            error_code_set=args.error_code_set,
            reachable_only=args.reachable_only,
            partial_writes=args.partial_writes,
            protocol=args.protocol,
            order=args.order,
        )

    if args.list_devices:
        for token in sorted(catalog.devices):
            dp_id = catalog.devices[token]
            display = _friendly(catalog._dp_name.get(dp_id, ""), catalog._text)
            n = len(catalog.events_for(token))
            row = catalog.identification_for(token)
            tail = ""
            if row.get("Identification"):
                tail = f"\tident={row['Identification']}"
                if row.get("IdentificationExtension"):
                    rng = row["IdentificationExtension"]
                    if row.get("IdentificationExtensionTill"):
                        rng += f"-{row['IdentificationExtensionTill']}"
                    tail += f" ext={rng}"
            print(f"{token}\t{display}\t({n} events){tail}")
        return 0

    device = args.device
    if not device and args.identify:
        if not (args.hw and args.sw):
            p.error("--identify requires --hw and --sw")
        ident_val = int(args.identify, 16)
        hw_val = int(args.hw, 16)
        sw_val = int(args.sw, 16)
        # One byte, or the two raw bytes the hub logs as F0=0xXXYY (identify.f0_bytes).
        f0_val = args.f0 or None
        token, why = catalog.resolve(ident_val, hw_val, sw_val, f0_val)
        if token is None:
            print(f"could not resolve device: {why}", file=sys.stderr)
            return 1
        print(f"resolved {args.identify}/HW{args.hw}/SW{args.sw} -> {token}  [{why}]", file=sys.stderr)
        # A "floor" match means the live software index is past every range the
        # catalog knows, so the newest revision is a best guess rather than a
        # match. Every GWG_VBEM row has an empty IdentificationExtensionTill,
        # which makes each range a single point, so GWG units commonly land
        # here -- and neighbouring revisions can disagree about what an
        # address holds.
        if "floor" in why:
            print(
                f"  WARNING: SW 0x{sw_val:02X} is beyond every range this catalog "
                f"declares for identification {args.identify}, so {token} is the newest known "
                f"revision rather than a match. Datapoints that changed meaning in a later "
                f"revision may be wrong. Verify any address you intend to WRITE against the "
                f"unit itself before trusting it.",
                file=sys.stderr,
            )
        device = token

    if not device:
        p.error("provide --device, or --identify with --hw/--sw (or use --list-devices)")

    text = generate(
        catalog,
        device,
        args.profile,
        args.include,
        args.exclude,
        emit_device_id=args.device_id,
        emit_error_history=args.error_history,
        error_codes=args.error_codes,
        error_code_set=args.error_code_set,
        reachable_only=args.reachable_only,
        partial_writes=args.partial_writes,
        order=args.order,
        protocol=args.protocol,
        gfa_chip=args.gfa_chip,
    )
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
