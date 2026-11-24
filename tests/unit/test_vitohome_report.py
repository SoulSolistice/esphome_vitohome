"""scripts/vitohome_report.py -- a field program's log into a table.

The parser takes the last `stats v1` line per label, the last `report v1`
header and the recorder dumps by trigger; the table has one row per label;
two runs compare label by label. The stats line's keys are VitoStats's
(vito_stats.h).
"""

import os
import pathlib
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

import vitohome_report as report  # noqa: E402

PROBE = """\
[10:00:00.100][I][diag:091]: report v1 program=probe-kw/1 protocol=KW esphome=2026.11.0 source=github://x/y@main sha=0123456789ab device="0x20CB VScotHO1"
[10:00:00.200][I][diag:092]: probe v1 start steps=2
\x1b[0;32m[10:00:10.000][I][vitohome.stats:974]: stats v1 KW base s=9 n=20 ok=20 timeout=0 length=0 nack=0 crc=0 error=0 \
device_error=0 ms=12/14/19 script_ok=0 script_failed=0 link.drained=0 link.retry=0 link.late=0 link.burst_off=0 \
link.nack=0 link.eot_refused=0 link.keepalive=0 link.eot=0 link.watchdog=0\x1b[0m
[10:00:20.000][I][vitohome.stats:974]: stats v1 KW enq_to_send:100 s=10 n=20 ok=18 timeout=2 length=0 nack=0 crc=0 \
error=0 device_error=0 ms=30/40/90 script_ok=0 script_failed=0 link.drained=3 link.retry=2 link.late=0 \
link.burst_off=0 link.nack=0 link.eot_refused=0 link.keepalive=0 link.eot=0 link.watchdog=0
[10:00:21.000][I][vitohome.recorder:377]: rec dump: 256 of 256 entries, frozen by a retry, 0 failure(s) since
[10:00:22.000][I][diag:099]: probe v1 done steps=2
"""

AUDIT_A = """\
[I][vitohome.stats:974]: stats v1 P300 audit s=600 n=100 ok=99 timeout=1 length=0 nack=0 crc=0 error=0 \
device_error=0 ms=50/60/70 script_ok=0 script_failed=0 link.nack=1
[I][vitohome.stats:974]: stats v1 P300 audit s=1200 n=200 ok=198 timeout=2 length=0 nack=0 crc=0 error=0 \
device_error=0 ms=50/61/80 script_ok=0 script_failed=0 link.nack=3
"""

AUDIT_B = """\
[I][vitohome.stats:974]: stats v1 P300 audit s=1200 n=200 ok=200 timeout=0 length=0 nack=0 crc=0 error=0 \
device_error=0 ms=-
"""


def test_header_steps_and_dumps():
    run = report.parse_log(PROBE, "probe.log")
    assert run.header["program"] == "probe-kw/1"
    assert run.header["device"] == "0x20CB VScotHO1"
    assert run.header["sha"] == "0123456789ab"
    assert list(run.steps) == ["base", "enq_to_send:100"]
    step = run.steps["enq_to_send:100"]
    assert step.protocol == "KW"
    assert step.num("timeout") == 2
    assert step.ms == (30, 40, 90)
    assert run.dumps == {"a retry": 1}
    assert run.done


def test_last_line_per_label_wins():
    run = report.parse_log(AUDIT_A)
    assert list(run.steps) == ["audit"]
    assert run.steps["audit"].num("n") == 200
    assert run.steps["audit"].values["link.nack"] == "3"


def test_table_rows():
    text = report.render(report.parse_log(PROBE, "probe.log"))
    assert "| base | 20 | 100.0 | 0 | 0 | 0 | 0 | 0 | 0 | 12/14/19 | 0/0 | - |" in text
    assert "| enq_to_send:100 | 20 | 90.0 | 2 |" in text
    assert "drained=3 retry=2" in text
    assert "1 frozen by a retry" in text
    assert "did not log" not in text
    assert "source `github://x/y@main` at `0123456789ab`" in text


def test_a_cut_probe_is_named():
    cut = PROBE.replace("probe v1 done steps=2", "")
    assert "did not log `probe v1 done`" in report.render(report.parse_log(cut))


def test_no_ms_without_an_answer():
    run = report.parse_log(AUDIT_B)
    assert run.steps["audit"].ms is None
    assert "| audit | 200 | 100.0 |" in report.render(run)


def test_compare_two_runs():
    text = report.compare(report.parse_log(AUDIT_A, "a"), report.parse_log(AUDIT_B, "b"))
    assert "| audit | 99.0 | 100.0 | 200 | 200 | 61 | - |" in text


def test_main_on_files(tmp_path, capsys):
    a = tmp_path / "a.log"
    b = tmp_path / "b.log"
    a.write_text(AUDIT_A, encoding="utf-8")
    b.write_text(AUDIT_B, encoding="utf-8")
    assert report.main([str(a), str(b)]) == 0
    out = capsys.readouterr().out
    assert "## a.log" in out and "## a.log against b.log" in out


def test_main_without_stats_fails(tmp_path):
    empty = tmp_path / "empty.log"
    empty.write_text("nothing here\n", encoding="utf-8")
    assert report.main([str(empty)]) == 1


def test_keys_match_the_stats_line():
    text = (pathlib.Path(_REPO_ROOT) / "components" / "vitohome" / "vito_stats.h").read_text(encoding="utf-8")
    for key in report.RESULTS:
        assert f'put("{key}"' in text
    assert 'char key[24] = "link.";' in text
