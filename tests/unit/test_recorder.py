"""`recorder:` -- the hub's ring of bytes and results (vito_recorder.h).

The size defaults to 256 entries on ESP8266 and 512 elsewhere, `after` to a
quarter of it; `freeze_on` takes timeout, error and retry, or manual alone;
`dump_on_freeze` dumps each frozen ring at once. The
bounds match VitoRecorder (test_limits_agree.py). The `recorder_dump` button
dumps it.
"""

import os
import pathlib
import subprocess
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM  # noqa: E402
from esphome.core import CORE  # noqa: E402

from components.vitohome import (  # noqa: E402
    CONF_AFTER,
    CONF_DUMP_ON_FREEZE,
    CONF_FREEZE_ON,
    CONF_SIZE,
    RECORDER_FREEZE,
    RECORDER_MAX_SIZE,
    RECORDER_MIN_SIZE,
    RECORDER_SCHEMA,
    RECORDER_SIZE_DEFAULT,
    RECORDER_SIZE_ESP8266,
)
from components.vitohome.button import CONFIG_SCHEMA as BUTTON_SCHEMA  # noqa: E402

_IMPORT = pathlib.Path(_REPO_ROOT) / "tests" / "native" / "sim" / "import_frames.py"


@pytest.fixture(autouse=True)
def _fresh_core():
    CORE.reset()
    yield
    CORE.reset()


def test_defaults_elsewhere():
    out = RECORDER_SCHEMA({})
    assert out[CONF_SIZE] == RECORDER_SIZE_DEFAULT
    assert out[CONF_AFTER] == RECORDER_SIZE_DEFAULT // 4
    assert out[CONF_FREEZE_ON] == ["timeout"]
    assert out[CONF_DUMP_ON_FREEZE] is False


def test_defaults_on_esp8266():
    CORE.data[KEY_CORE] = {KEY_TARGET_PLATFORM: "esp8266"}
    out = RECORDER_SCHEMA({})
    assert out[CONF_SIZE] == RECORDER_SIZE_ESP8266
    assert out[CONF_AFTER] == RECORDER_SIZE_ESP8266 // 4


@pytest.mark.parametrize("size", [RECORDER_MIN_SIZE, RECORDER_MAX_SIZE])
def test_size_bounds_taken(size):
    assert RECORDER_SCHEMA({CONF_SIZE: size})[CONF_SIZE] == size


@pytest.mark.parametrize("size", [RECORDER_MIN_SIZE - 1, RECORDER_MAX_SIZE + 1])
def test_size_out_of_bounds_refused(size):
    with pytest.raises(cv.Invalid):
        RECORDER_SCHEMA({CONF_SIZE: size})


def test_after_below_size():
    assert RECORDER_SCHEMA({CONF_SIZE: 64, CONF_AFTER: 63})[CONF_AFTER] == 63
    with pytest.raises(cv.Invalid, match="less than"):
        RECORDER_SCHEMA({CONF_SIZE: 64, CONF_AFTER: 64})


def test_freeze_on_takes_both_and_one():
    assert RECORDER_SCHEMA({CONF_FREEZE_ON: ["timeout", "error"]})[CONF_FREEZE_ON] == ["timeout", "error"]
    assert RECORDER_SCHEMA({CONF_FREEZE_ON: "error"})[CONF_FREEZE_ON] == ["error"]
    assert RECORDER_SCHEMA({CONF_FREEZE_ON: "manual"})[CONF_FREEZE_ON] == ["manual"]
    assert RECORDER_SCHEMA({CONF_FREEZE_ON: ["retry", "timeout"]})[CONF_FREEZE_ON] == ["retry", "timeout"]


def test_dump_on_freeze_needs_a_trigger():
    assert RECORDER_SCHEMA({CONF_FREEZE_ON: "retry", CONF_DUMP_ON_FREEZE: True})[CONF_DUMP_ON_FREEZE] is True
    with pytest.raises(cv.Invalid, match="needs a trigger"):
        RECORDER_SCHEMA({CONF_FREEZE_ON: "manual", CONF_DUMP_ON_FREEZE: True})


def test_manual_stands_alone():
    with pytest.raises(cv.Invalid, match="manual"):
        RECORDER_SCHEMA({CONF_FREEZE_ON: ["manual", "timeout"]})


def test_unknown_trigger_refused():
    with pytest.raises(cv.Invalid):
        RECORDER_SCHEMA({CONF_FREEZE_ON: "resync"})


def test_trigger_bits_match_the_header():
    text = (pathlib.Path(_REPO_ROOT) / "components" / "vitohome" / "vito_recorder.h").read_text(encoding="utf-8")
    assert "RECORDER_FREEZE_TIMEOUT = 1U << 0" in text
    assert "RECORDER_FREEZE_ERROR = 1U << 1" in text
    assert "RECORDER_FREEZE_RETRY = 1U << 2" in text
    assert RECORDER_FREEZE == {"timeout": 1, "error": 2, "retry": 4, "manual": 0}


def test_dump_button():
    cfg = BUTTON_SCHEMA({"name": "Recorder Dump", "type": "recorder_dump"})
    assert str(cfg["icon"]) == "mdi:record-rec"
    assert str(cfg["entity_category"]) == "diagnostic"


def test_import_frames_reads_the_first_dump(tmp_path):
    log = tmp_path / "dump.log"
    log.write_bytes(
        b"[10:00:00.002][I][vitohome.recorder:430]: rec dump: 4 of 512 entries, frozen by an error, 0 failure(s) since\n"
        b"[10:00:00.003][I][vitohome.recorder:430]: rec +0.000 p1 >>> 41\n"
        b"[10:00:00.004][I][vitohome.recorder:430]: rec +250.623 p2 >>> 05:00:01:00:F8:02:00\n"
        b"[10:00:00.005][I][vitohome.recorder:430]: rec +262.100 p3 <<< 15:15\n"
        b"[10:00:00.006][I][vitohome.recorder:430]: rec +262.100 p3 = script nack @00F8\n"
        b"[10:00:01.002][I][vitohome.recorder:430]: rec dump: 1 of 512 entries, frozen by the dump, 0 failure(s) since\n"
        b"[10:00:01.003][I][vitohome.recorder:430]: rec +0.000 p9 <<< 05\n"
        b"[10:00:01.004][D][vitohome.frames:194]: >>> 04\n"
    )
    out = subprocess.run(
        [sys.executable, "-I", str(_IMPORT), str(log), "dump", "--source", "test"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    assert "{0, false, 0, 1}," in out
    assert "{251, false, 1, 7}," in out
    assert "{262, true, 8, 2}," in out
    assert out.count("false,") + out.count("true,") == 3
