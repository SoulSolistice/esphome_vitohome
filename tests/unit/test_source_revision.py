"""component_revision() -- the commit the firmware is built from.

12 hex digits for a clean checkout, "-dirty" when a file of the component
differs or is not committed, "unknown" for a folder no repository tracks (a
copy inside another repository) and outside any repository.
"""

import os
import re
import subprocess
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from components.vitohome import REVISION_UNKNOWN, component_revision  # noqa: E402


def _git(cwd, *args):
    subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", *args],
        cwd=cwd,
        check=True,
        capture_output=True,
    )


@pytest.fixture
def checkout(tmp_path):
    comp = tmp_path / "components" / "vitohome"
    comp.mkdir(parents=True)
    (comp / "__init__.py").write_text("# component\n", encoding="utf-8")
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "add", ".")
    _git(tmp_path, "commit", "-q", "-m", "c")
    return tmp_path, comp


def test_a_clean_checkout_names_its_commit(checkout):
    _, comp = checkout
    assert re.fullmatch(r"[0-9a-f]{12}", component_revision(comp))


def test_a_changed_or_new_file_is_dirty(checkout):
    _, comp = checkout
    clean = component_revision(comp)
    (comp / "__init__.py").write_text("# changed\n", encoding="utf-8")
    assert component_revision(comp) == f"{clean}-dirty"
    (comp / "__init__.py").write_text("# component\n", encoding="utf-8")
    (comp / "new.h").write_text("// new\n", encoding="utf-8")
    assert component_revision(comp) == f"{clean}-dirty"


def test_a_change_outside_the_component_is_not(checkout):
    root, comp = checkout
    clean = component_revision(comp)
    (root / "README.md").write_text("elsewhere\n", encoding="utf-8")
    assert component_revision(comp) == clean


def test_an_untracked_copy_inside_a_repository_is_unknown(checkout):
    root, _ = checkout
    copy = root / "copy" / "vitohome"
    copy.mkdir(parents=True)
    (copy / "__init__.py").write_text("# copy\n", encoding="utf-8")
    assert component_revision(copy) == REVISION_UNKNOWN


def test_outside_a_repository_is_unknown(tmp_path, monkeypatch):
    plain = tmp_path / "plain"
    plain.mkdir()
    (plain / "__init__.py").write_text("# plain\n", encoding="utf-8")
    monkeypatch.setenv("GIT_CEILING_DIRECTORIES", str(tmp_path))
    assert component_revision(plain) == REVISION_UNKNOWN


def test_without_git_it_is_unknown(checkout, monkeypatch):
    _, comp = checkout
    monkeypatch.setenv("PATH", "")
    assert component_revision(comp) == REVISION_UNKNOWN


def test_this_checkout_has_a_revision():
    assert re.fullmatch(r"([0-9a-f]{12}(-dirty)?)|unknown", component_revision())
