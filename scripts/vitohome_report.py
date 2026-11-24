#!/usr/bin/env python3
"""Read the log of a field program (example/diagnostics/README.md) into a table.

    python scripts/vitohome_report.py run.log            one run
    python scripts/vitohome_report.py a.log b.log        two runs, step by step

A log is what `esphome logs` printed, with or without its colours and time
stamps. The table takes the last `stats v1` line of each label (an audit logs
the counts since boot again and again), the last `report v1` header, and
counts the recorder dumps by what froze them. Markdown on stdout.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys

_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_LINE = re.compile(r"\b(report|stats|probe) v1 (.*)$")
_PAIR = re.compile(r'(\S+?)=("[^"]*"|\S*)')
_DUMP = re.compile(r"rec dump: (\d+) of (\d+) entries, frozen by (.+?), (\d+) failure")

# The result keys of a stats line, in its order; everything with a `link.`
# prefix is a fact of the link.
RESULTS = ("ok", "timeout", "length", "nack", "crc", "error", "device_error")


@dataclass
class Step:
    protocol: str
    label: str
    values: dict[str, str]

    def num(self, key: str) -> int:
        try:
            return int(self.values.get(key, "0"))
        except ValueError:
            return 0

    @property
    def ms(self) -> tuple[int, int, int] | None:
        parts = self.values.get("ms", "-").split("/")
        if len(parts) != 3:
            return None
        try:
            low, avg, high = (int(p) for p in parts)
        except ValueError:
            return None
        return low, avg, high


@dataclass
class Run:
    name: str
    header: dict[str, str] = field(default_factory=dict)
    steps: dict[str, Step] = field(default_factory=dict)
    dumps: dict[str, int] = field(default_factory=dict)
    done: bool = False


def _pairs(text: str) -> dict[str, str]:
    return {k: v.strip('"') for k, v in _PAIR.findall(text)}


def parse_log(text: str, name: str = "log") -> Run:
    run = Run(name)
    for raw in text.splitlines():
        line = _ANSI.sub("", raw)
        dump = _DUMP.search(line)
        if dump:
            trigger = dump.group(3)
            run.dumps[trigger] = run.dumps.get(trigger, 0) + 1
            continue
        m = _LINE.search(line)
        if not m:
            continue
        kind, rest = m.groups()
        if kind == "report":
            run.header = _pairs(rest)
        elif kind == "probe":
            run.done = run.done or rest.startswith("done")
        else:
            words = rest.split(" ", 2)
            if len(words) < 3:
                continue
            protocol, label, values = words
            run.steps[label] = Step(protocol, label, _pairs(values))
    return run


def _percent(part: int, whole: int) -> str:
    return "-" if whole == 0 else f"{100.0 * part / whole:.1f}"


def _facts(step: Step) -> str:
    out = [f"{k[5:]}={v}" for k, v in step.values.items() if k.startswith("link.") and v not in ("0", "")]
    return " ".join(out) or "-"


def render(run: Run) -> str:
    h = run.header
    lines = [f"## {run.name}", ""]
    if h:
        lines.append(
            f"program `{h.get('program', '?')}`, protocol {h.get('protocol', '?')}, ESPHome {h.get('esphome', '?')}, "
            f"source `{h.get('source', '?')}` at `{h.get('sha', '?')}`, device `{h.get('device', '?')}`"
        )
    else:
        lines.append("no `report v1` header")
    if run.steps and not run.done and any(not s.label.startswith(("audit", "now")) for s in run.steps.values()):
        lines.append("")
        lines.append("the probe did not log `probe v1 done`: it stopped early or the log is cut")
    lines += [
        "",
        "| step | n | ok % | " + " | ".join(RESULTS[1:]) + " | ms min/avg/max | timed ok/failed | link facts |",
        "|---|---|---|" + "---|" * (len(RESULTS) - 1) + "---|---|---|",
    ]
    for step in run.steps.values():
        n = step.num("n")
        timed = f"{step.num('script_ok')}/{step.num('script_failed')}"
        ms = step.ms
        cells = [
            step.label,
            str(n),
            _percent(step.num("ok"), n),
            *(str(step.num(k)) for k in RESULTS[1:]),
            "-" if ms is None else f"{ms[0]}/{ms[1]}/{ms[2]}",
            timed,
            _facts(step),
        ]
        lines.append("| " + " | ".join(cells) + " |")
    if run.dumps:
        lines += ["", "recorder dumps: " + ", ".join(f"{n} frozen by {t}" for t, n in sorted(run.dumps.items()))]
    return "\n".join(lines) + "\n"


def compare(a: Run, b: Run) -> str:
    lines = [
        f"## {a.name} against {b.name}",
        "",
        "| step | ok % A | ok % B | n A | n B | ms avg A | ms avg B |",
        "|---|---|---|---|---|---|---|",
    ]
    labels = list(a.steps) + [label for label in b.steps if label not in a.steps]
    for label in labels:
        sa, sb = a.steps.get(label), b.steps.get(label)

        def cell(step: Step | None, what: str) -> str:
            if step is None:
                return "-"
            if what == "ok":
                return _percent(step.num("ok"), step.num("n"))
            if what == "n":
                return str(step.num("n"))
            ms = step.ms
            return "-" if ms is None else str(ms[1])

        lines.append(
            f"| {label} | {cell(sa, 'ok')} | {cell(sb, 'ok')} | {cell(sa, 'n')} | {cell(sb, 'n')} | "
            f"{cell(sa, 'ms')} | {cell(sb, 'ms')} |"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+", type=Path, help="one log, or two to compare")
    args = parser.parse_args(argv)
    if len(args.logs) > 2:
        parser.error("one log, or two to compare")
    runs = [parse_log(p.read_text(encoding="utf-8", errors="replace"), p.name) for p in args.logs]
    out = "\n".join(render(r) for r in runs)
    if len(runs) == 2:
        out += "\n" + compare(*runs)
    sys.stdout.write(out)
    return 0 if all(r.steps for r in runs) else 1


if __name__ == "__main__":
    sys.exit(main())
