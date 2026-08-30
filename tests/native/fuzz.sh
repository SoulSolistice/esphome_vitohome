#!/usr/bin/env bash
# Coverage-guided fuzzing campaign for the VS2 parser. Human-initiated.
#
# Not a gate and NOT run by CI. CI executes the same target in its
# deterministic replay build (build_and_run.sh -> fuzz_parser_vs2), which walks
# the committed seed corpus in a fraction of a second. What lives here is the
# open-ended search: it runs for as long as you let it, its result depends on
# the seed and the clock, and a campaign that finds nothing is not evidence
# worth spending a CI minute on. The two builds share one source file
# (fuzz_parser_vs2.cpp) so the oracles cannot drift apart.
#
# Requires clang: -fsanitize=fuzzer is a compiler-rt feature with no GCC
# equivalent, which is why the CI-side build stays on g++ and this one does
# not. Ubuntu 24.04's clang 18 is sufficient.
#
#   bash fuzz.sh                 # 5 minutes, one worker per core
#   bash fuzz.sh 3600            # an hour
#   bash fuzz.sh 300 2           # 5 minutes, 2 workers
#
# WHAT TO DO WITH A FINDING. libFuzzer writes the reproducer to
# $FUZZ_DIR/findings/ and prints the path. First replay it against the CI build
# to confirm it is real and not an artefact of the fuzzer's own instrumentation:
#
#   g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
#       -fno-sanitize-recover=all -I../../components/vitohome \
#       -I../../components/vitohome/optolink fuzz_parser_vs2.cpp \
#       ../../components/vitohome/optolink/protocol/vs2/parser_vs2.cpp \
#       ../../components/vitohome/optolink/protocol/vs2/packet_vs2.cpp \
#       ../../components/vitohome/optolink/constants.cpp -o /tmp/replay
#   /tmp/replay <path-to-crash-file>
#
# Then minimise it (`-minimize_crash=1 -runs=100000 <file>`) and add the reduced
# bytes to fuzz_corpus_vs2.h with a comment saying what it broke. That is the
# step that converts a campaign into a permanent gate; a crash file left in
# /tmp is a finding that will be rediscovered from scratch next year.
set -euo pipefail

# Resolved before anything else so the script works from any cwd; the relative
# ROOT default below is then relative to tests/native, as in build_and_run.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SECS="${1:-300}"
WORKERS="${2:-$(nproc)}"
ROOT="${VITOHOME_ROOT:-../../components/vitohome}"
OPTO="$ROOT/optolink"
FUZZ_DIR="${FUZZ_DIR:-${TMPDIR:-/tmp}/vitohome-fuzz}"

if ! command -v clang++ >/dev/null 2>&1; then
  echo "fuzz.sh needs clang++ (-fsanitize=fuzzer has no GCC equivalent)." >&2
  echo "  sudo apt install clang llvm" >&2
  exit 1
fi

CORPUS="$FUZZ_DIR/corpus"
FINDINGS="$FUZZ_DIR/findings"
mkdir -p "$CORPUS" "$FINDINGS"

# Materialise the committed seeds as files. libFuzzer wants a directory; the
# corpus lives in a header so it stays reviewable and survives the text-typed
# pre-commit hooks (see fuzz_corpus_vs2.h). This is the one place the two
# representations meet, and it is a pure export -- the header is the source of
# truth and is never written back to. Corpus entries the fuzzer discovers
# accumulate alongside them and are deliberately NOT committed: they are a
# cache that makes the next campaign start warm, not a gate.
python3 - "$CORPUS" "$SCRIPT_DIR/fuzz_corpus_vs2.h" <<'PY'
import os, re, sys
out, header = sys.argv[1], sys.argv[2]
src = open(header).read()
n = 0
for m in re.finditer(r"kSeed_(\w+)\[\] = \{(.*?)\};", src, re.S):
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
    open(os.path.join(out, "seed_" + m.group(1)), "wb").write(data)
    n += 1
print(f"  {n} seeds exported to {out}")
PY

echo "== building the libFuzzer target (clang) =="
clang++ -std=c++17 -g -Wall -Wextra -Werror \
  -DVITOHOME_FUZZ_LIBFUZZER \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
  -I"$ROOT" -I"$OPTO" \
  fuzz_parser_vs2.cpp \
  "$OPTO/protocol/vs2/parser_vs2.cpp" \
  "$OPTO/protocol/vs2/packet_vs2.cpp" \
  "$OPTO/constants.cpp" \
  -o "$FUZZ_DIR/fuzz_parser_vs2"

echo "== campaign: ${SECS}s, ${WORKERS} worker(s), corpus $CORPUS =="
# Run from $FUZZ_DIR rather than the source tree: with -jobs, libFuzzer writes
# a fuzz-<N>.log per worker into the CURRENT directory, so launching from
# tests/native litters the checkout with untracked logs. Every path handed to
# the binary below is absolute, so the cd costs nothing.
cd "$FUZZ_DIR"
set +e
"$FUZZ_DIR/fuzz_parser_vs2" "$CORPUS" \
  -artifact_prefix="$FINDINGS/" \
  -max_total_time="$SECS" \
  -workers="$WORKERS" -jobs="$WORKERS" \
  -print_final_stats=1
rc=$?
set -e

found=$(find "$FINDINGS" -type f | wc -l)
echo
if [ "$found" -gt 0 ]; then
  echo "FINDINGS: $found reproducer(s) in $FINDINGS"
  find "$FINDINGS" -type f -printf '  %p (%s bytes)\n'
  echo "Replay one against the CI build, minimise it, then promote it into fuzz_corpus_vs2.h."
  exit 1
fi
echo "no findings. corpus is now $(find "$CORPUS" -type f | wc -l) units."
exit $rc
