"""Device identification -> catalog lookup (scripts/gen_catalog.py, web/lookup.js).

``Catalog.resolve_pool()`` holds the matching rules. ``build_resolve_index()``
enumerates its answers into ``resolve.json`` and ``web/lookup.js`` reads that
table. These tests pin the rules, prove the table reproduces them for every
identification byte, run the JavaScript against the same table, and guard the
committed ``example/catalogs/{de,en}`` sets against drifting from their
manifests and from each other.

Run under any Python (gen_catalog is stdlib-only)::

    python -m pytest tests/unit/test_resolve_index.py -q
"""

import csv
import json
import os
import shutil
import subprocess
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402

_FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")
_CATALOG_DIR = os.path.join(_REPO_ROOT, "example", "catalogs")
# One folder per shipped language (scripts/export_catalogs.sh); resolve.json is
# language-independent and lives in _CATALOG_DIR itself.
_CATALOG_LANGS = ("de", "en")
_LOOKUP_JS = os.path.join(_REPO_ROOT, "web", "lookup.js")


def _row(token, ident, ext=None, extt=None, f0=None, f0t=None):
    return {
        "token": token,
        "Identification": f"{ident:04X}",
        "IdentificationExtension": None if ext is None else f"{ext:04X}",
        "IdentificationExtensionTill": None if extt is None else f"{extt:04X}",
        "ident": ident,
        "ext": ext,
        "extt": extt,
        "f0": f0,
        "f0t": f0t,
    }


# Shapes taken from the real export: a boiler family with contiguous ranges and
# an F0-subdivided top range (0x20CB), a family keyed by hardware index with
# single-value software entries (0x2053), a family mixing ranged and unranged
# rows (0x208A), a serial-keyed family (0x2000) and a lone device (0x20A3).
_IDENT_ROWS = [
    _row("Boiler", 0x20CB, 0x0100, 0x0103),
    _row("Boiler_4", 0x20CB, 0x0104, 0x0147),
    _row("Boiler_72", 0x20CB, 0x0148, 0x0159),
    _row("Boiler_200_10", 0x20CB, 0x01C8, 0x01FF, f0=0x10, f0t=0x10),
    _row("Boiler_200", 0x20CB, 0x01C8, 0x01FF, f0=0x00, f0t=0x00),
    _row("Boiler_200_01", 0x20CB, 0x01C8, 0x01FF, f0=0x01, f0t=0x09),
    _row("GWG_A_00", 0x2053, 0x0100),
    _row("GWG_A_21", 0x2053, 0x0121),
    _row("GWG_B_03", 0x2053, 0x0203),
    _row("Com", 0x208A),
    _row("Com_10", 0x208A, 0x010A, 0x010F),
    _row("Serial_1", 0x2000, 0x064200),
    _row("Serial_2", 0x2000, 0x074200),
    _row("Lone", 0x20A3),
]


@pytest.fixture(scope="module")
def catalog():
    return gc.Catalog({}, {}, {}, {}, {}, {}, {}, _IDENT_ROWS, {})


def _manifest():
    return [{"token": r["token"], "file": r["token"].lower() + ".yaml", "status": "ok"} for r in _IDENT_ROWS]


# --- the matching rules -----------------------------------------------------


@pytest.mark.parametrize(
    "ident,hw,sw,f0,token,reason",
    [
        (0x20CB, 0x01, 0x51, None, "Boiler_72", "software-index range"),
        # HW 0x03 is what the reference Vitodens reports; the family lists 0x01 only.
        (0x20CB, 0x03, 0x51, None, "Boiler_72", "HW 0x03 not in catalog"),
        (0x20CB, 0x01, 0x60, None, "Boiler_72", "software-index floor"),
        (0x20CB, 0x01, 0xC8, 0x10, "Boiler_200_10", "F0 match"),
        (0x20CB, 0x01, 0xC8, 0x05, "Boiler_200_01", "F0 match"),
        (0x20CB, 0x01, 0xC8, None, "Boiler_200", "F0 not provided"),
        (0x20CB, 0x01, 0xC8, 0x77, "Boiler_200", "no F0 match"),
        # The two raw bytes the hub reads at 0x00F0/0x00F1 (F0=0xXXYY): tried
        # 0x00F0 first, then 0x00F1; the reason says which one decided.
        (0x20CB, 0x01, 0xC8, "1000", "Boiler_200_10", "F0 match (0x00F0)"),
        (0x20CB, 0x01, 0xC8, "7710", "Boiler_200_10", "F0 match (0x00F1)"),
        (0x20CB, 0x01, 0xC8, "0510", "Boiler_200_01", "F0 match (0x00F0)"),
        (0x20CB, 0x01, 0xC8, "0x0010", "Boiler_200", "F0 match (0x00F0)"),
        (0x20CB, 0x01, 0xC8, "7777", "Boiler_200", "no F0 match"),
        # Two hex digits stay one byte, as a person types the catalog's F0 value.
        (0x20CB, 0x01, 0xC8, "10", "Boiler_200_10", "F0 match"),
        # A family F0 does not subdivide ignores it.
        (0x20CB, 0x01, 0x51, "3000", "Boiler_72", "software-index range"),
        (0x2053, 0x01, 0x7A, None, "GWG_A_21", "software-index floor"),
        (0x2053, 0x02, 0x00, None, "GWG_B_03", "lowest software index"),
        (0x208A, 0x01, 0x0C, None, "Com_10", "software-index range"),
        (0x20A3, 0x01, 0x00, None, "Lone", "Identification match"),
    ],
)
def test_resolve(catalog, ident, hw, sw, f0, token, reason):
    got, why = catalog.resolve(ident, hw, sw, f0)
    assert got == token
    assert reason in why


@pytest.mark.parametrize(
    "value,expected",
    [
        (None, None),
        (0x10, [0x10]),
        (0x3010, [0x30, 0x10]),
        ("10", [0x10]),
        ("0x10", [0x10]),
        ("0010", [0x00, 0x10]),
        ("3000", [0x30, 0x00]),
        ("12345", None),
        ("zz", None),
        ("", None),
    ],
)
def test_f0_bytes_tells_one_byte_from_the_two_raw_ones(value, expected):
    assert gc.f0_bytes(value) == expected


def test_resolve_unknown_ident(catalog):
    token, why = catalog.resolve(0x1234, 1, 0)
    assert token is None
    assert "0x1234" in why


def test_resolve_serial_keyed_family_is_not_guessed(catalog):
    # A 3-byte extension is not HW<<8|SW; picking one of the rows by comparing
    # its low byte with the software index would be a silent wrong match.
    token, why = catalog.resolve(0x2000, 0x01, 0x00)
    assert token is None
    assert "Serial_1" in why and "Serial_2" in why


def test_resolve_pool_reports_how(catalog):
    assert catalog.resolve_pool(0x20CB, 1, 0x51)[1] == "range"
    assert catalog.resolve_pool(0x20CB, 1, 0x60)[1] == "floor"
    assert catalog.resolve_pool(0x2053, 2, 0x00)[1] == "lowest"
    assert catalog.resolve_pool(0x2000, 1, 0x00)[1] == "family"
    # hw=None is "hardware index unknown": matched on software index, no note.
    pool, how, _why, note = catalog.resolve_pool(0x20CB, None, 0x51)
    assert (pool[0]["token"], how, note) == ("Boiler_72", "range", "")


# --- the precomputed table --------------------------------------------------


def test_index_shape(catalog):
    index = gc.build_resolve_index(catalog, _manifest())
    assert index["schema"] == gc.RESOLVE_SCHEMA
    assert sorted(index["idents"]) == ["2000", "2053", "208A", "20A3", "20CB"]
    assert index["idents"]["2000"] == {"family": ["Serial_1", "Serial_2"]}
    assert index["idents"]["20A3"] == {"family": ["Lone"]}
    assert sorted(index["idents"]["2053"]["hw"]) == ["01", "02"]
    assert index["idents"]["208A"]["unranged"] == ["Com"]
    assert index["tokens"]["Boiler_72"] == {"file": "boiler_72.yaml", "status": "ok", "detail": "ok"}
    assert index["tokens"]["GWG_A_21"] == {"file": "gwg_a_21.yaml", "status": "ok", "detail": "ok"}


def test_index_runs_cover_every_software_index(catalog):
    index = gc.build_resolve_index(catalog, _manifest())
    for entry in index["idents"].values():
        if "family" in entry:
            continue
        for runs in [*entry["hw"].values(), entry["any"]]:
            assert runs[0]["sw"][0] == 0 and runs[-1]["sw"][1] == 255
            for a, b in zip(runs, runs[1:], strict=False):
                assert b["sw"][0] == a["sw"][1] + 1


def test_index_candidates_keep_resolve_order(catalog):
    index = gc.build_resolve_index(catalog, _manifest())
    top = index["idents"]["20CB"]["hw"]["01"][-1]
    assert top["sw"] == [0xC8, 0xFF]
    assert top["token"] == "Boiler_200"  # the generic (lowest F0) variant
    assert [c["token"] for c in top["candidates"]] == ["Boiler_200_10", "Boiler_200", "Boiler_200_01"]
    assert top["candidates"][2]["f0"] == [0x01, 0x09]


def test_dump_is_stable_json(catalog):
    index = gc.build_resolve_index(catalog, _manifest())
    text = gc._dump_resolve_index(index)
    assert json.loads(text) == index
    assert text.endswith("}\n") and "\r" not in text
    assert text == gc._dump_resolve_index(gc.build_resolve_index(catalog, _manifest()))


# --- web/lookup.js against the same table -----------------------------------

_NODE = shutil.which("node")

_NODE_DRIVER = """
const { lookup, parseIdentification } = require(process.argv[1]);
const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const answers = input.cases.map(([id, hw, sw, f0]) => {
  const r = lookup(input.index, id, hw, sw, f0);
  return r.kind === 'match' ? r.token : r.kind === 'family' && r.tokens.length === 1 ? r.tokens[0].token : null;
});
process.stdout.write(JSON.stringify({
  answers,
  parsed: input.lines.map(parseIdentification),
  unknown_without_id: lookup(input.index, null, null, null, null),
}));
"""

_LOG_LINES = [
    "[09:25:58.421][C][vitohome:773]:   Device: 0x2053 (GWG_VBEM) HW=0x01 SW=0x7A",
    "[I][vitohome:1409]: Device identification: 0x20CB (VScotHO1) HW=0x03 SW=0x51",
    "read 0x5525 ok\n[C][vitohome:773]:   Device: 0x20CB HW=0x03 SW=0x51 F0=0x10",
    "0x20A3",
    "nothing here",
    # A pasted log without a Device line: an address in it is not a device id.
    "read 0x5525 ok\nRaw: 0x00F8: 20",
    # The burner-control chip, which selects the GFA companion.
    "Device: 0x20CB (VScotHO1) HW=0x03 SW=0x51 GFA=0x24",
    # Exactly as the hub logs it at boot (ident_string_), chip name included.
    "[I][vitohome:1771]: Device identification: 0x20CB (VScotHO1) HW=0x03 SW=0x51 GFA=0x41 (ÖFA)",
    # The raw F0 bytes at 0x00F0/0x00F1, as ident_string_ appends them.
    "[I][vitohome:1828]: Device identification: 0x20CB (VScotHO1) HW=0x01 SW=0xC8 GFA=0x24 (NEC) F0=0x1000",
]


@pytest.mark.skipif(_NODE is None, reason="node is not installed")
def test_javascript_lookup_matches_resolve(catalog):
    index = gc.build_resolve_index(catalog, _manifest())
    cases = [
        (ident, hw, sw, f0)
        for ident in (0x20CB, 0x2053, 0x208A, 0x2000, 0x20A3, 0x1234)
        for hw in (0x01, 0x02, 0x03)
        for sw in range(256)
        for f0 in (None, 0x00, 0x05, 0x10, 0x77, "10", "1000", "7710", "0010", "7777")
    ]
    done = subprocess.run(
        [_NODE, "-e", _NODE_DRIVER, _LOOKUP_JS],
        input=json.dumps({"index": index, "cases": cases, "lines": _LOG_LINES}),
        capture_output=True,
        text=True,
        check=False,
    )
    assert done.returncode == 0, done.stderr
    out = json.loads(done.stdout)
    expected = [catalog.resolve(*case)[0] for case in cases]
    assert out["answers"] == expected
    assert out["parsed"] == [
        {"id": 0x2053, "hw": 0x01, "sw": 0x7A, "f0": None, "gfa": None},
        {"id": 0x20CB, "hw": 0x03, "sw": 0x51, "f0": None, "gfa": None},
        # F0 comes back as text: its digit count tells one byte from two.
        {"id": 0x20CB, "hw": 0x03, "sw": 0x51, "f0": "10", "gfa": None},
        {"id": 0x20A3, "hw": None, "sw": None, "f0": None, "gfa": None},
        {"id": None, "hw": None, "sw": None, "f0": None, "gfa": None},
        {"id": None, "hw": None, "sw": None, "f0": None, "gfa": None},
        {"id": 0x20CB, "hw": 0x03, "sw": 0x51, "f0": None, "gfa": 0x24},
        {"id": 0x20CB, "hw": 0x03, "sw": 0x51, "f0": None, "gfa": 0x41},
        {"id": 0x20CB, "hw": 0x01, "sw": 0xC8, "f0": "1000", "gfa": 0x24},
    ]
    assert out["unknown_without_id"] == {"kind": "unknown", "ident": None}


# --- bulk export ------------------------------------------------------------


def _export(catalog, out_dir, **kwargs):
    return gc.export_all(
        catalog,
        str(out_dir),
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
        **kwargs,
    )


def _index_rows(out_dir):
    with open(os.path.join(out_dir, "index.csv"), encoding="utf-8", newline="") as fh:
        raw = fh.read()
    return raw, {r["token"]: r for r in csv.DictReader(raw.splitlines())}


def test_export_all_manifest_uses_lf_and_writes_lookup(tmp_path):
    catalog = gc.load_catalog(_FIXTURE_DIR)
    assert _export(catalog, tmp_path) == 0
    raw, rows = _index_rows(tmp_path)
    assert "\r" not in raw
    lookup = json.loads((tmp_path / gc.RESOLVE_FILENAME).read_text(encoding="utf-8"))
    assert lookup["tokens"]["VTestHO1_99"] == {"file": rows["VTestHO1_99"]["file"], "status": "ok", "detail": "ok"}


# --- the committed catalog set ----------------------------------------------


@pytest.mark.parametrize("lang", _CATALOG_LANGS)
def test_committed_catalogs_agree_with_manifest_and_lookup(lang):
    lang_dir = os.path.join(_CATALOG_DIR, lang)
    _raw, rows = _index_rows(lang_dir)
    with open(os.path.join(_CATALOG_DIR, gc.RESOLVE_FILENAME), encoding="utf-8") as fh:
        lookup = json.load(fh)
    assert lookup["schema"] == gc.RESOLVE_SCHEMA

    on_disk = {f for f in os.listdir(lang_dir) if f.endswith(".yaml")}
    listed = {r["file"] for r in rows.values() if r["status"] == "ok"}
    # A row marked ok whose file is missing sends a user to a dead link; a file
    # with no ok row is unreachable from the finder.
    assert listed == on_disk
    # The finder serves both languages from one lookup table, so a language
    # must not carry a file the other lacks.
    other = os.path.join(_CATALOG_DIR, next(x for x in _CATALOG_LANGS if x != lang))
    assert on_disk == {f for f in os.listdir(other) if f.endswith(".yaml")}
    # No generated catalog may linger at the top level (the pre-0008 layout).
    assert not [f for f in os.listdir(_CATALOG_DIR) if f.endswith(".yaml")]

    assert set(lookup["tokens"]) == set(rows)
    for token, row in rows.items():
        expected = {"file": row["file"], "status": row["status"].split(":", 1)[0], "detail": row["status"]}
        if row.get("gfa_slots"):
            expected["gfa"] = True
        assert lookup["tokens"][token] == expected

    # Every chip the finder offers has its companion in this language.
    gfa_dir = os.path.join(lang_dir, gc.GFA_DIRNAME)
    on_disk_gfa = set(os.listdir(gfa_dir)) if os.path.isdir(gfa_dir) else set()
    assert on_disk_gfa == {gc.gfa_companion_name(chip) for chip in lookup.get("gfa_chips", {})}

    referenced = set()
    for entry in lookup["idents"].values():
        runs = [] if "family" in entry else [*[r for v in entry["hw"].values() for r in v], *entry["any"]]
        referenced.update(entry.get("family", []), entry.get("unranged", []))
        for run in runs:
            referenced.add(run["token"])
            referenced.update(c["token"] for c in run.get("candidates", []))
    assert referenced <= set(rows)
