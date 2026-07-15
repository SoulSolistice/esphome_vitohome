#!/usr/bin/env bash
# Gate: line coverage of components/vitohome/decode.h under tests/native/test_decode.cpp.
#
# WHY ONLY decode.h
# -----------------
# This is deliberately NOT a repo-wide coverage number, and that is the whole
# design. The component's own C++ is ~4600 lines, of which ~3600 (vitohome.cpp,
# vito_*.cpp) is bound to esphome::Component and CANNOT execute on a build host
# by construction. A repo-wide percentage would therefore be dominated by code
# that is intentionally unexecutable here, and would create exactly two bad
# incentives: test the untestable, or add exclusions until the number is a
# fiction. A misleading metric is worse than no metric.
#
# decode.h is the opposite case, and the one file that earns a gate:
#   * pure C++ (includes only <cmath> <cstddef> <cstdint> <cstdio> <cstring>) --
#     no ESPHome, no framework, 100% host-executable;
#   * it is where a missed branch means a WRONG NUMBER on a boiler rather than
#     a compile error -- sign extension, scaling, BCD, Schaltzeiten, UTF-16.
#
# WHY gcov AND NOT gcovr/lcov/codecov
# -----------------------------------
# gcov ships with gcc, which the `native` CI job already uses to build this
# exact test. No pip package, no third-party action, no SaaS token, no upload.
# The .gcov line format parsed below (`<count>:<lineno>:<source>`) is stable and
# the parse is checked against gcov's own summary on every run (see VERIFY
# below), so a format drift fails loudly instead of silently reporting 100%.
#
# THE THRESHOLD IS A RATCHET, NOT A TARGET
# ----------------------------------------
# MIN_LINE_PCT is set to the coverage that already held when the gate was
# introduced. It exists to stop regression, not to claim sufficiency. Raise it
# when tests are added; never lower it to make a red build green.
#
# Usage: bash scripts/coverage_decode.sh [MIN_LINE_PCT]
set -euo pipefail

cd "$(dirname "$0")/.."

MIN_LINE_PCT="${1:-89}"
SRC="$PWD/tests/native/test_decode.cpp"
TARGET="decode.h"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Everything below runs with CWD = $WORK and an ABSOLUTE source path. That is
# not incidental: g++ writes the .gcno next to the -o output and the binary
# writes its .gcda into the CWD it was launched from, while gcov only finds
# them relative to its own CWD. Compiling from the repo root and running gcov
# in $WORK scatters the three across two directories and gcov silently reports
# nothing. Keep the compile, the run, and the gcov invocation in one directory.
#
# -O0 so gcov line attribution is not scrambled by inlining; decode.h is all
# inline functions in a header, so at -O2 they would be attributed to the
# caller. --coverage implies -fprofile-arcs -ftest-coverage.
# -Werror matches the rest of the host gates (see tests/native/build_and_run.sh).
cd "$WORK"
g++ -std=c++17 -Wall -Wextra -Werror --coverage -O0 -o test_decode "$SRC"
./test_decode

# -b gives branch data (reported below for information; only lines are gated).
gcov -b -c test_decode.cpp > gcov_summary.txt 2>&1

GCOV_FILE="$WORK/$TARGET.gcov"
if [ ! -f "$GCOV_FILE" ]; then
  echo "ERROR: gcov produced no $TARGET.gcov. Did $SRC stop including $TARGET?"
  echo "       Refusing to report a vacuous pass."
  sed 's/^/    /' gcov_summary.txt
  exit 1
fi

# Parse the .gcov line format. Field 1 (before the first ':') is the execution
# count, right-aligned:
#     '-'      -> not an executable line (comment, decl, blank)
#     '#####'  -> executable, NEVER executed
#     '====='  -> executable, unexecutable-block marker
#     '<N>'    -> executed N times
#     '<N>*'   -> executed, but some blocks within the line were not
# Lines beginning 'branch'/'call'/'function' are annotations, not source lines.
read -r total covered uncovered_list <<<"$(
  awk -F: '
    {
      c = $1
      gsub(/^[ \t]+|[ \t]+$/, "", c)
      if (c ~ /^(branch|call|function)/) next
      if (c == "-" || c == "") next
      if (c == "#####" || c == "=====") { total++; unc = unc " " $2; next }
      if (c ~ /^[0-9]+\*?$/) { total++; covered++; next }
    }
    END { printf "%d %d %s", total, covered, unc }
  ' "$GCOV_FILE"
)"

if [ "$total" -eq 0 ]; then
  echo "ERROR: parsed 0 executable lines from $GCOV_FILE -- the parse broke."
  exit 1
fi

PCT="$(awk -v c="$covered" -v t="$total" 'BEGIN { printf "%.2f", (c * 100.0) / t }')"

# VERIFY: cross-check our parse against gcov's own summary for this file. If the
# two disagree, the .gcov format drifted and the number above is not
# trustworthy -- fail rather than gate on a bad parse.
GCOV_PCT="$(awk -v tgt="$TARGET" '
  $0 ~ ("File .*" tgt "'\''") { want = 1; next }
  want && /^Lines executed:/ { sub(/^Lines executed:/, ""); sub(/%.*/, ""); print; exit }
' gcov_summary.txt)"
if [ -z "$GCOV_PCT" ]; then
  echo "ERROR: could not read gcov's own summary line for $TARGET, so the parse"
  echo "       above is unverified. An unfired cross-check is worse than none:"
  echo "       failing rather than gating on an unconfirmed number."
  sed 's/^/    /' gcov_summary.txt
  exit 1
fi
if [ "$GCOV_PCT" != "$PCT" ]; then
  echo "ERROR: parse disagrees with gcov's own summary ($PCT vs $GCOV_PCT)."
  echo "       The .gcov format likely changed; fix the parser in this script."
  exit 1
fi

BRANCH_INFO="$(awk -v tgt="$TARGET" '
  $0 ~ ("File .*" tgt "'\''") { want = 1; next }
  want && /^Branches executed:/ { print; exit }
' gcov_summary.txt)"

echo "=============================================="
echo " decode.h coverage (host, tests/native/test_decode.cpp)"
echo "=============================================="
echo "  executable lines : $total"
echo "  covered          : $covered"
echo "  uncovered        : $((total - covered))"
echo "  line coverage    : ${PCT}%   (gate: >= ${MIN_LINE_PCT}%)"
[ -n "$BRANCH_INFO" ] && echo "  $BRANCH_INFO  (informational; not gated)"
echo ""
echo "  uncovered decode.h source lines:${uncovered_list:- none}"
echo ""

if awk -v p="$PCT" -v m="$MIN_LINE_PCT" 'BEGIN { exit !(p + 0 < m + 0) }'; then
  echo "FAIL: decode.h line coverage ${PCT}% is below the ${MIN_LINE_PCT}% ratchet."
  echo "      This gate is a ratchet: it may only ever be RAISED. If a change"
  echo "      genuinely removes tested code, raise the covered fraction back"
  echo "      instead of lowering the threshold."
  exit 1
fi

echo "OK: decode.h line coverage ${PCT}% meets the ${MIN_LINE_PCT}% ratchet."
