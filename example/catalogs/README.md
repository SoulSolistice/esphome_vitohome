# Generated device catalogs

One ready-to-include ESPHome package per Viessmann unit, generated from the
Vitosoft 2026 XML export by `scripts/gen_catalog.py --export-all`. These are
**include-only fragments** (no top-level `esphome:` block) — pull one into a
standalone config via `packages:` / `!include`; see [`../README.md`](../README.md)
for a complete wiring example.

**Find your unit in [`index.csv`](index.csv)**: look up the `ident` value the
hub logs at boot (e.g. `0x20CB`) or the Vitosoft `token`, then include the
`file` named in that row. Rows with `status: skipped` have no file — every
datapoint on that unit needs a non-Optolink access method.

> **Fault-code caveat.** `--export-all` attaches the default `vd300`
> (Vitodens 300-W B3HA) fault-code map to *every* unit. Fault-code semantics
> are device-variant-specific; for any unit other than the reference one,
> regenerate with the correct `--error-code-set` (see the note in
> [`../README.md`](../README.md)).

This stub is hand-written and survives regeneration: `gen_catalog.py` creates
the directory with `exist_ok=True` and never clears it.
