"""The browser editor's YAML parser (web/editor.js parseCatalogYaml) keeps
every key of an entity.

External review, 2026-10-28: a blank line ended the entity, so a valid entity
with a blank line between its name and its address exported without the
address -- "Load local YAML" was not lossless. The parser now keeps an entity
open across blank lines; the next entity, a section or a group ends it, and a
comment after a blank line still belongs to the next entity.

The last test parses every shipped catalog and checks that each scalar key of
each entity block comes back.
"""

import glob
import json
import os
import re
import shutil
import subprocess

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_EDITOR_JS = os.path.join(_REPO_ROOT, "web", "editor.js")
_NODE = shutil.which("node")

_DRIVER = r"""
const { parseCatalogYaml } = require(process.argv[1]);
const texts = JSON.parse(require("fs").readFileSync(0, "utf8"));
const out = texts.map((t) =>
  parseCatalogYaml(t).entities.map((e) => ({ props: e.props, notes: e.notes, tags: e.tags }))
);
process.stdout.write(JSON.stringify(out));
"""

pytestmark = pytest.mark.skipif(_NODE is None, reason="node is not installed")


def _parse(*texts):
    done = subprocess.run(
        [_NODE, "-e", _DRIVER, _EDITOR_JS],
        input=json.dumps(list(texts)),
        capture_output=True,
        text=True,
        check=False,
    )
    assert done.returncode == 0, done.stderr
    return json.loads(done.stdout)


def test_a_blank_line_inside_an_entity_keeps_its_keys():
    text = """sensor:
  - platform: vitohome
    name: "Outside"

    address: 0x5525
    length: 2
"""
    (entities,) = _parse(text)
    assert len(entities) == 1
    assert entities[0]["props"]["address"] == "0x5525"
    assert entities[0]["props"]["length"] == "2"


def test_a_comment_after_a_blank_line_belongs_to_the_next_entity():
    text = """sensor:
  - platform: vitohome
    name: "One"
    address: 0x0001

  # vito: quickcheck
  - platform: vitohome
    name: "Two"
    address: 0x0002
"""
    (entities,) = _parse(text)
    assert [e["props"]["name"] for e in entities] == ["One", "Two"]
    assert entities[1]["tags"] == ["quickcheck"]
    assert entities[0]["tags"] == []


def test_a_section_still_ends_an_entity():
    text = """sensor:
  - platform: vitohome
    name: "One"

number:
  - platform: vitohome
    name: "Two"
"""
    (entities,) = _parse(text)
    assert [(e["props"]["component"], e["props"]["name"]) for e in entities] == [("sensor", "One"), ("number", "Two")]


_CATALOGS = sorted(glob.glob(os.path.join(_REPO_ROOT, "example", "catalogs", "*", "*.yaml")))


def _expected_scalars(text):
    """Per entity block, the scalar keys at the entity's own indentation."""
    blocks, block = [], None
    for line in text.splitlines():
        if re.match(r"^\s*-\s+(platform|id):", line):
            if block is not None:
                blocks.append(block)
            block = {}
            continue
        if re.match(r"^[a-z_]+:\s*$", line):
            if block is not None:
                blocks.append(block)
            block = None
            continue
        if block is None:
            continue
        m = re.match(r"^\s{4}([a-zA-Z0-9_]+):\s*(\S.*)$", line)
        if m and not m.group(2).startswith(("&", "*", "#")):
            block[m.group(1)] = True
    if block is not None:
        blocks.append(block)
    return blocks


@pytest.mark.skipif(not _CATALOGS, reason="no shipped catalogs")
def test_every_shipped_catalog_round_trips_its_keys():
    texts = []
    for path in _CATALOGS:
        with open(path, encoding="utf-8") as f:
            texts.append(f.read())
    parsed = _parse(*texts)
    missing = []
    for path, text, entities in zip(_CATALOGS, texts, parsed, strict=True):
        expected = _expected_scalars(text)
        if len(expected) != len(entities):
            missing.append(f"{os.path.basename(path)}: {len(entities)} entities parsed, {len(expected)} in the file")
            continue
        for keys, ent in zip(expected, entities, strict=True):
            missing.extend(
                f"{os.path.basename(path)}: {ent['props'].get('name')} lost {k}" for k in keys if k not in ent["props"]
            )
    assert not missing, "\n".join(missing[:20])
