"""Turn the frame lines of an ESPHome log into a trace header (sim_trace.h).

    python import_frames.py LOG NAME --source TEXT [--start HEX] [--lines N] > trace_NAME.h

Reads the >>> and <<< lines of `uart_debug` and `vitohome.frames`, whose bytes
are written as colon-separated hex, with their [hh:mm:ss.mmm] times. A log
with a recorder dump (`vitohome.recorder`, vito_recorder.h) gives the dump's
lines instead, with their times from the first entry, rounded to the ms. --start
skips to the first engine line with those bytes (04 for a P300 log, so the
replay begins with the session handshake); --lines keeps that many lines.
The header holds bytes and times only, nothing else of the log.
"""

import argparse
import re
import sys

LINE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]\[D\]\[(?:uart_debug|vitohome\.frames)[^\]]*\]: (>>>|<<<) ([0-9A-Fa-f:]+)\s*$"
)
REC = re.compile(r"\]\[vitohome\.recorder[^\]]*\]: rec \+(\d+)\.(\d{3}) p\d+ (>>>|<<<) ([0-9A-Fa-f:]+)\s*$")
DAY_MS = 24 * 3600 * 1000


def read_lines(path):
    with open(path, encoding="utf-8", errors="replace") as log:
        raws = [raw.strip() for raw in log]
    # The first recorder dump; its times restart at every dump.
    recorded = []
    dumps = 0
    for raw in raws:
        if "]: rec dump:" in raw and "vitohome.recorder" in raw:
            dumps += 1
        m = REC.search(raw)
        if m and dumps == 1:
            ms, us, direction, hexes = m.groups()
            data = bytes(int(x, 16) for x in hexes.split(":") if x)
            if data:
                recorded.append((int(ms) + (int(us) >= 500), direction == "<<<", data))
    if recorded:
        return recorded
    out = []
    for raw in raws:
        m = LINE.match(raw)
        if not m:
            continue
        h, mi, s, ms, direction, hexes = m.groups()
        t = ((int(h) * 60 + int(mi)) * 60 + int(s)) * 1000 + int(ms)
        data = bytes(int(x, 16) for x in hexes.split(":") if x)
        if data:
            out.append((t, direction == "<<<", data))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("name")
    ap.add_argument("--source", required=True, help="where the log comes from, for the header comment")
    ap.add_argument("--start", help="hex bytes of the engine line to start at, e.g. 04")
    ap.add_argument("--lines", type=int, default=0)
    args = ap.parse_args()
    if not re.fullmatch(r"[a-z0-9_]+", args.name):
        sys.exit("NAME: lower case, digits and _ only")

    lines = read_lines(args.log)
    if args.start:
        want = bytes.fromhex(args.start)
        first = next((i for i, (_, dev, data) in enumerate(lines) if not dev and data == want), None)
        if first is None:
            sys.exit(f"no engine line {args.start}")
        lines = lines[first:]
    if args.lines:
        lines = lines[: args.lines]
    if not lines:
        sys.exit("no frame lines")

    t0 = lines[0][0]
    flat = bytearray()
    rows = []
    previous = t0
    day = 0
    for t, dev, data in lines:
        if t < previous:
            day += DAY_MS  # past midnight
        previous = t
        rows.append((t + day - t0, dev, len(flat), len(data)))
        flat += data

    n = args.name
    out = sys.stdout
    out.reconfigure(newline="\n")  # the repository's line ends, on Windows too
    out.write("#pragma once\n\n")
    out.write(f"// {args.source}\n")
    out.write(f"// {len(rows)} frame lines, imported by import_frames.py: bytes and times only.\n\n")
    out.write('#include "sim_trace.h"\n\n')
    out.write(f"inline constexpr uint8_t kTraceBytes_{n}[] = {{\n")
    for i in range(0, len(flat), 16):
        out.write("    " + ", ".join(f"0x{b:02X}" for b in flat[i : i + 16]) + ",\n")
    out.write("};\n\n")
    out.write(f"inline constexpr sim::TraceLine kTraceLines_{n}[] = {{\n")
    for t, dev, off, length in rows:
        out.write(f"    {{{t}, {'true' if dev else 'false'}, {off}, {length}}},\n")
    out.write("};\n\n")
    out.write(
        f'inline constexpr sim::Trace kTrace_{n} = {{"{n}", "{args.source}", kTraceLines_{n},\n'
        f"                                        sizeof(kTraceLines_{n}) / sizeof(kTraceLines_{n}[0]), "
        f"kTraceBytes_{n}}};\n"
    )


if __name__ == "__main__":
    main()
