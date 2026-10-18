"""Bulk export (--export-all), manifest (index.csv), and lookup table (resolve.json).

One catalog file per device token. The Vitosoft token IDs already follow
Viessmann's ``unit_swIndex[_variant]`` naming -- ``VScotHO1_72`` is unit
``VScotHO1`` at software index 72 (its IdentificationExtension low byte 0x48 =
72), and hardware variants are *distinct* unit names (``GWG_VBES`` vs
``GWG_VWMS`` vs ``GWG_VBT2``), not a numeric suffix. So the file name is just
the lower-cased token.
"""

from __future__ import annotations

import csv
import os
import re
import sys

import fault_codes

from .constants import _MANIFEST_COLUMNS, GFA_DIRNAME, RESOLVE_FILENAME
from .identify import _dump_resolve_index, _is_hw_sw_ext, build_resolve_index as _build_resolve_index
from .loader import Catalog
from .renderer import generate
from .special import gfa_chip_names, gfa_companion, gfa_companion_name

_FILENAME_SAFE_RE = re.compile(r"[^a-z0-9._-]+")


def _export_stem(token: str) -> str:
    """Filesystem-safe, lower-cased file stem for a device token: lower-case it
    and replace any character outside ``[a-z0-9._-]`` so the name is portable.
    ``VScotHO1_72`` -> ``vscotho1_72``."""
    stem = _FILENAME_SAFE_RE.sub("_", token.strip().lower())
    return stem.strip("._-") or "device"


def _identity_fields(row: dict) -> dict:
    """Identification indices for one ``ecnDataPointType`` row, for the manifest.

    ``ident`` is the device Identification (0xF8F9). The hardware index and the
    software-index range are reported ONLY when the extension is the 2-byte
    ``HW<<8 | SW`` boiler/controller form (as in ``resolve_pool()``): the high
    byte is the categorical hardware generation, the low byte the software
    index. A handful of device types -- M-Bus meters, some telecom modules --
    carry a longer serial-number extension instead; for those the numeric
    columns stay blank and the raw extension string is kept so the row is still
    traceable. Everything here is read straight from the export -- nothing is
    inferred.
    """
    ext = row.get("ext")
    extt = row.get("extt")
    ident = row.get("ident")
    out = {
        "ident": f"0x{ident:04X}" if ident is not None else "",
        "hw_index": "",
        "sw_lo": "",
        "sw_hi": "",
        "f0_lo": "",
        "f0_hi": "",
        "ext_raw": row.get("IdentificationExtension") or "",
    }
    if _is_hw_sw_ext(ext):
        out["hw_index"] = ext >> 8
        out["sw_lo"] = ext & 0xFF
        out["sw_hi"] = (extt & 0xFF) if (extt is not None and 0 <= extt <= 0xFFFF) else ext & 0xFF
    f0 = row.get("f0")
    if f0 is not None:
        f0t = row.get("f0t")
        out["f0_lo"] = f0
        out["f0_hi"] = f0t if f0t is not None else f0
    return out


def build_resolve_index(catalog: Catalog, manifest: list[dict]) -> dict:
    """Precomputed identification lookup for the web catalog finder (see
    identify.build_resolve_index), with the GFA chip names."""
    return _build_resolve_index(catalog, manifest, gfa_chips_fn=gfa_chip_names)


def export_all(
    catalog: Catalog,
    out_dir: str,
    *,
    profile: str,
    include_re: str | None,
    exclude_re: str | None,
    token_filter: str | None,
    suffix: str,
    emit_device_id: bool,
    emit_error_history: bool,
    error_codes: bool,
    error_code_set: str,
    reachable_only: bool,
    protocol: str | None = None,
    order: str = "address",
    partial_writes: bool = True,
) -> int:
    """Write one catalog per device token into *out_dir* (created if needed).

    Files are named ``<token><suffix>`` (lower-cased, sanitised) -- Viessmann's
    ``unit_swIndex[_variant]`` key -- and an ``index.csv`` manifest is written
    alongside, mapping every file back to its identification signature (ident /
    hardware index / software-index range / F0 range / event count / status).
    ``resolve.json`` (see ``build_resolve_index``) carries the same mapping
    precomputed per identification byte, for the web catalog finder. With GFA
    fault texts in the export, the GFA companion packages go to
    ``<out_dir>/gfa/``.

    Each device is generated with the SAME options the single-device path uses,
    so ``--export-all`` is "run the generator for every unit with these flags".
    A unit that fails to generate is recorded and skipped; it does not abort the
    batch. Returns a process exit code (0 if at least one file was written, 1
    otherwise).
    """
    if os.path.exists(out_dir) and not os.path.isdir(out_dir):
        print(f"--out {out_dir!r} exists and is not a directory", file=sys.stderr)
        return 1
    os.makedirs(out_dir, exist_ok=True)

    pat = re.compile(token_filter) if token_filter else None
    ident_by_token = {r["token"]: r for r in catalog._ident if r.get("token")}

    tokens = sorted(catalog.devices)
    if pat:
        tokens = [t for t in tokens if pat.search(t)]
    if not tokens:
        where = f" matching {token_filter!r}" if token_filter else ""
        print(f"no device tokens{where} to export", file=sys.stderr)
        return 1

    if error_codes and error_code_set in fault_codes.SETS:
        print(
            f"note: --export-all attaches the same fault-code map ({error_code_set!r}) to "
            "every unit, but fault-code semantics are device-variant-specific -- use the "
            "default --error-code-set auto (Vitosoft's per-device texts) instead.",
            file=sys.stderr,
        )

    manifest: list[dict] = []
    gfa_slot_ids: list[str] | None = None  # the GFA slot ids every GFA catalog shares
    used: dict[str, str] = {}  # stem -> token, to catch sanitiser collisions
    written = skipped = failed = 0

    for token in tokens:
        row = ident_by_token.get(token, {})
        n_events = len(catalog.events_for(token))
        rec = {
            "file": "",
            "token": token,
            **_identity_fields(row),
            "events": n_events,
            "entities": "",
            "bytes": "",
            "status": "",
        }

        if n_events == 0:
            rec["status"] = "skipped: no events"
            manifest.append(rec)
            skipped += 1
            continue

        stem = _export_stem(token)
        if used.get(stem, token) != token:  # distinct token collided after sanitising
            n = 2
            while f"{stem}-{n}" in used:
                n += 1
            stem = f"{stem}-{n}"
        used[stem] = token
        fname = stem + suffix

        try:
            gen_stats: dict = {}
            text = generate(
                catalog,
                token,
                profile,
                include_re,
                exclude_re,
                emit_device_id=emit_device_id,
                emit_error_history=emit_error_history,
                error_codes=error_codes,
                error_code_set=error_code_set,
                reachable_only=reachable_only,
                partial_writes=partial_writes,
                order=order,
                protocol=protocol,
                stats=gen_stats,
            )
        except SystemExit as exc:  # generate() uses this for "no emittable events"
            rec["status"] = f"skipped: {exc}"
            manifest.append(rec)
            skipped += 1
            continue
        except Exception as exc:  # noqa: BLE001 -- one bad unit must not abort the batch
            rec["status"] = f"error: {type(exc).__name__}: {exc}"
            manifest.append(rec)
            failed += 1
            continue

        rec["entities"] = gen_stats.get("entities", "")
        # A device with NO real entity (only the hub-fed device_id, plus at
        # most some custom-decode / unreachable comments) is not an Optolink
        # unit: every genuine datapoint of a KBUS-only Dekamatik cascade or an
        # M-Bus meter needs an access method VitoWiFi cannot drive, and such a
        # file would be useless. Comments alone never make a usable catalog, so
        # the skip keys on the real-entity count, not on `entities` (which
        # counts the device_id) or the comment count.
        if gen_stats.get("real_entities", 0) == 0:
            rec["status"] = "skipped: no Optolink-reachable datapoints"
            manifest.append(rec)
            skipped += 1
            continue

        data = text.encode("utf-8")
        with open(os.path.join(out_dir, fname), "wb") as fh:
            fh.write(data)
        rec["file"] = fname
        rec["bytes"] = len(data)
        rec["status"] = "ok"
        ids = gen_stats.get("gfa_ids") or []
        if ids:
            rec["gfa_slots"] = len(ids)
            # One companion serves every GFA catalog only through the slot ids
            # they share (gfa_fehler_01..20 in the current export); keep the
            # common ones, which every catalog can be extended by.
            gfa_slot_ids = list(ids) if gfa_slot_ids is None else [i for i in gfa_slot_ids if i in ids]
        manifest.append(rec)
        written += 1

    manifest.sort(key=lambda r: r["token"])
    index_path = os.path.join(out_dir, "index.csv")
    with open(index_path, "w", encoding="utf-8", newline="") as fh:
        # LF, like every other generated file: the csv module defaults to CRLF.
        w = csv.DictWriter(fh, fieldnames=_MANIFEST_COLUMNS, lineterminator="\n")
        w.writeheader()
        for rec in manifest:
            w.writerow({k: rec.get(k, "") for k in _MANIFEST_COLUMNS})

    resolve_path = os.path.join(out_dir, RESOLVE_FILENAME)
    with open(resolve_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_dump_resolve_index(build_resolve_index(catalog, manifest)))

    companions = 0
    _per, _gen, gfa = catalog.fault_texts()
    if gfa_slot_ids and gfa:
        gfa_dir = os.path.join(out_dir, GFA_DIRNAME)
        os.makedirs(gfa_dir, exist_ok=True)
        for chip in sorted(gfa):
            with open(os.path.join(gfa_dir, gfa_companion_name(chip)), "w", encoding="utf-8", newline="\n") as fh:
                fh.write(gfa_companion(catalog, chip, gfa_slot_ids))
            companions += 1
        print(f"wrote {companions} GFA companion(s) to {gfa_dir}", file=sys.stderr)

    print(
        f"wrote {written} catalog(s) to {out_dir}  (skipped {skipped}, failed {failed}); "
        f"manifest: {index_path}; lookup: {resolve_path}",
        file=sys.stderr,
    )
    if failed:
        print(f"warning: {failed} unit(s) failed to generate -- see 'error:' rows in index.csv", file=sys.stderr)
    return 0 if written else 1
