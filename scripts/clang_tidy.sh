#!/usr/bin/env bash
# clang-tidy over the host-buildable code, with the checks in .clang-tidy.
#
# Covers the optolink engine sources and, through tests/native/tidy_headers.cpp,
# the hub headers that build without ESPHome -- once per protocol, since
# protocol_select.h picks the engine by build flag. Any finding fails the run.
#
# Usage, from anywhere:
#   bash scripts/clang_tidy.sh
#   CLANG_TIDY=/path/to/clang-tidy bash scripts/clang_tidy.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TIDY="${CLANG_TIDY:-clang-tidy}"
COMP=components/vitohome
OPTO="$COMP/optolink"
ENGINE=(
  "$OPTO/constants.cpp"
  "$OPTO/protocol/vs2/vs2.cpp"
  "$OPTO/protocol/vs2/parser_vs2.cpp"
  "$OPTO/protocol/vs2/packet_vs2.cpp"
  "$OPTO/protocol/vs1/vs1.cpp"
  "$OPTO/protocol/vs1/packet_vs1.cpp"
  "$OPTO/protocol/gwg/gwg.cpp"
  "$OPTO/protocol/gwg/packet_gwg.cpp"
)
FLAGS=(-std=c++17 -I"$COMP" -I"$OPTO")

"$TIDY" --version | head -2
"$TIDY" --quiet "${ENGINE[@]}" -- "${FLAGS[@]}"
for proto in "" -DVITOHOME_PROTOCOL_KW -DVITOHOME_PROTOCOL_GWG; do
  echo "== hub headers ${proto:-(P300)}"
  "$TIDY" --quiet tests/native/tidy_headers.cpp -- "${FLAGS[@]}" ${proto:+"$proto"}
done
echo "clang-tidy OK"
