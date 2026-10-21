"""Hardware/software/F0 identification matching and precomputed index builder."""

from __future__ import annotations

import json

from .constants import RESOLVE_SCHEMA


def _is_hw_sw_ext(ext: int | None) -> bool:
    """True when an IdentificationExtension is the 2-byte ``HW<<8 | SW`` form.

    Boilers and controllers carry that pair (0xFA, 0xFB). A handful of device
    types carry a longer serial- or module-style extension instead, which the
    two live bytes cannot be compared against.
    """
    return ext is not None and 0 <= ext <= 0xFFFF


def resolve_pool(
    ident_rows: list[dict],
    device_id: int,
    hw: int | None,
    sw: int,
) -> tuple[list[dict] | None, str, str, str]:
    """Candidate datapoint types for live identification bytes, before F0.

    The shared core of ``resolve_device()`` and of the precomputed lookup table
    (``build_resolve_index``), so the CLI and the web finder cannot drift.

    Algorithm:
      1. Filter by ``Identification == device_id`` -> the device family.
      2. Only a 2-byte IdentificationExtension is a ``HW<<8 | SW`` pair.
         Longer extensions (Dekamatik, M-Bus, telecom modules) are serial- or
         module-style keys that 0xFA/0xFB cannot select between: such a
         family is returned whole, as ``how == "family"``.
      3. The high byte of IdentificationExtension is a *categorical* hardware-
         generation tag (only one device id in the export uses more than one
         high byte). So:
           * if the live HW byte appears among the family's entries, keep only
             those entries (equality);
           * otherwise (e.g. HW=0x03 while the family is all 0x01, or
             ``hw is None``) the byte is the family's constant nominal value
             -> drop it and match on the software index alone. Folding HW
             into a (HW<<8|SW) magnitude comparison would inflate the value
             past the SW ranges and select a far-later revision.
      4. Match the SOFTWARE index (low byte) against each entry's ext..extt
         range; "from software index N onwards" semantics. If SW is beyond all
         known ranges, pick the latest (floor); if below all, the lowest.

    Returns ``(pool, how, why, note)``. *pool* is the list of matching
    identification rows (``None`` when the device id is unknown, in which case
    *why* is the failure reason); *how* is one of ``"family"``, ``"range"``,
    ``"floor"``, ``"lowest"``; *note* is non-empty when the hardware byte was
    not found in the catalog and was ignored.
    """
    fam = [t for t in ident_rows if t["ident"] == device_id]
    if not fam:
        return None, "", f"no datapoint type with Identification == 0x{device_id:04X}", ""
    have_ext = [t for t in fam if _is_hw_sw_ext(t["ext"])]
    if not have_ext:
        return fam, "family", "Identification match (no HW/SW IdentificationExtension in catalog)", ""

    highs = {t["ext"] >> 8 for t in have_ext}
    note = ""
    if hw in highs:
        cand = [t for t in have_ext if (t["ext"] >> 8) == hw]
    else:
        cand = have_ext
        if hw is not None:
            note = f" (HW 0x{hw:02X} not in catalog; matched on software index only)"

    # Ignore the rare multi-high-byte spans; keep single-high-byte ranges.
    single = [t for t in cand if t["extt"] is None or (t["ext"] >> 8) == (t["extt"] >> 8)]
    cand = single or cand

    def sw_range(t: dict) -> tuple[int, int]:
        lo = t["ext"] & 0xFF
        hi = (t["extt"] & 0xFF) if t["extt"] is not None else lo
        return lo, hi

    contain = [t for t in cand if sw_range(t)[0] <= sw <= sw_range(t)[1]]
    if contain:
        return contain, "range", "software-index range", note
    below = sorted([t for t in cand if sw_range(t)[0] <= sw], key=lambda t: sw_range(t)[0])
    if below:
        return [below[-1]], "floor", "software-index floor (SW beyond catalog ranges; latest revision)", note
    lowest = min(cand, key=lambda t: sw_range(t)[0])
    return [lowest], "lowest", "lowest software index (SW below catalog ranges)", note


def generic_of(pool: list[dict]) -> dict:
    """The row resolve_device() falls back to when F0 cannot pick one.

    Rows without an F0 range sort first, then the lowest F0 -- the generic
    variant of a software-index range that F0 subdivides.
    """
    if any(t["f0"] is not None for t in pool):
        return min(pool, key=lambda t: t["f0"] if t["f0"] is not None else -1)
    return pool[0]


def resolve_device(
    ident_rows: list[dict],
    device_id: int,
    hw: int,
    sw: int,
    f0: int | None = None,
) -> tuple[str | None, str]:
    """Map live identification bytes to a datapoint-type token.

    ``resolve_pool()`` narrows the family by hardware and software index. If
    several entries share the software-index range, F0 -- the value at 0x00F0,
    Vitosoft's "Erweiterte Gerätekennung (F0)" -- disambiguates; without F0, the generic variant
    is preferred.

    Returns (token, reason) or (None, reason).
    """
    pool, how, why, note = resolve_pool(ident_rows, device_id, hw, sw)
    if pool is None:
        return None, why
    if how == "family":
        if len(pool) > 1:
            names = ", ".join(t["token"] for t in pool[:6]) + (", ..." if len(pool) > 6 else "")
            return None, (
                f"{len(pool)} datapoint types share Identification 0x{device_id:04X} and are not "
                f"distinguished by hardware/software index: {names}"
            )
        return pool[0]["token"], why

    f0_aware = [t for t in pool if t["f0"] is not None]
    if f0_aware:
        if f0 is not None:
            fm = [t for t in f0_aware if t["f0"] <= f0 <= (t["f0t"] if t["f0t"] is not None else t["f0"])]
            if fm:
                return fm[0]["token"], why + " + F0 match" + note
        tail = " (F0 not provided; generic variant)" if f0 is None else " (no F0 match; generic variant)"
        return generic_of(pool)["token"], why + tail + note
    return pool[0]["token"], why + note


def _resolve_run_key(pool: list[dict], how: str) -> tuple:
    return (how, tuple(t["token"] for t in pool))


def _resolve_runs(catalog, device_id: int, hw: int | None) -> list[dict]:
    """Run-length-encoded resolve_pool() results over software index 0..255."""
    runs: list[dict] = []
    prev_key = None
    for sw in range(256):
        pool, how, _why, _note = catalog.resolve_pool(device_id, hw, sw)
        key = _resolve_run_key(pool, how)
        if key == prev_key:
            runs[-1]["sw"][1] = sw
            continue
        prev_key = key
        run: dict = {"sw": [sw, sw], "how": how, "token": generic_of(pool)["token"]}
        if len(pool) > 1:
            # Pool order is preserved: resolve_device() takes the FIRST F0 match.
            run["candidates"] = [
                {
                    "token": t["token"],
                    "f0": None if t["f0"] is None else [t["f0"], t["f0t"] if t["f0t"] is not None else t["f0"]],
                }
                for t in pool
            ]
        runs.append(run)
    return runs


def build_resolve_index(catalog, manifest: list[dict], gfa_chips_fn=None) -> dict:
    """Precomputed identification lookup for the web catalog finder.

    Every answer ``resolve_pool()`` can give is enumerated here, so the finder
    only does a table lookup and carries no copy of the matching rules. Per
    device id (``"20CB"``):

      * ``family`` -- tokens of a family that 0xFA/0xFB cannot subdivide; or
      * ``hw`` -- per catalogued hardware index (``"01"``), the software-index
        runs; ``any`` -- the runs used when the live hardware byte is not one
        of those (the byte is then ignored); ``unranged`` -- family members
        that carry no HW/SW range and therefore never match by range.

    A run is ``{"sw": [lo, hi], "how": "range"|"floor"|"lowest", "token": T}``
    where *token* is what ``resolve_device()`` returns without F0;
    ``candidates`` (in resolve order, with their F0 ranges) is present when F0
    decides.

    ``tokens`` maps each exported token to its catalog file and status.
    ``gfa_chips`` ({chip: name}, from *gfa_chips_fn*) is present when some
    catalog has GFA history slots.
    """
    idents: dict[str, dict] = {}
    for device_id in sorted({r["ident"] for r in catalog._ident if r["ident"] is not None}):
        fam = [r for r in catalog._ident if r["ident"] == device_id]
        ranged = [r for r in fam if _is_hw_sw_ext(r["ext"])]
        if not ranged:
            idents[f"{device_id:04X}"] = {"family": [r["token"] for r in fam]}
            continue
        entry: dict = {
            "hw": {f"{hw:02X}": _resolve_runs(catalog, device_id, hw) for hw in sorted({r["ext"] >> 8 for r in ranged})},
            "any": _resolve_runs(catalog, device_id, None),
        }
        unranged = [r["token"] for r in fam if not _is_hw_sw_ext(r["ext"])]
        if unranged:
            entry["unranged"] = unranged
        idents[f"{device_id:04X}"] = entry

    tokens = {}
    for rec in manifest:
        entry = {"file": rec["file"], "status": rec["status"].split(":", 1)[0], "detail": rec["status"]}
        if rec.get("gfa_slots"):
            entry["gfa"] = True  # takes a GFA companion (gfa/chip_<XX>.yaml)
        tokens[rec["token"]] = entry
    out: dict = {"schema": RESOLVE_SCHEMA, "idents": idents, "tokens": tokens}
    if gfa_chips_fn is not None and any(rec.get("gfa_slots") for rec in manifest):
        out["gfa_chips"] = gfa_chips_fn(catalog)
    return out


def _dump_resolve_index(index: dict) -> str:
    """Stable, diff-friendly JSON: one device id / one token per line."""

    def block(mapping: dict) -> str:
        rows = [
            f"    {json.dumps(k)}: {json.dumps(mapping[k], ensure_ascii=False, separators=(',', ':'))}"
            for k in sorted(mapping)
        ]
        return "{\n" + ",\n".join(rows) + "\n  }"

    return (
        "{\n"
        f'  "schema": {index["schema"]},\n'
        f'  "idents": {block(index["idents"])},\n'
        + (
            f'  "gfa_chips": {json.dumps(index["gfa_chips"], ensure_ascii=False, separators=(",", ":"))},\n'
            if "gfa_chips" in index
            else ""
        )
        + f'  "tokens": {block(index["tokens"])}\n'
        "}\n"
    )
