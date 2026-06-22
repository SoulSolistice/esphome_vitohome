#!/usr/bin/env bash
# Host build + run of the P300 (VS2) transaction harness against the in-tree
# vendored Optolink engine (components/vitohome/optolink/).
#
# ROOT = component root that contains optolink/. The test includes the engine as
#   #include "optolink/optolink.h"
# so the component dir is on the include path; the engine's own headers use
# paths relative to their own location, which resolve from there.
#
# Only the P300 path is compiled (OptolinkEngine<P300> == VS2Engine); the VS1/GWG
# translation units are deferred protocols and not referenced by these vectors.
set -euo pipefail
ROOT="${1:-../../components/vitohome}"
OPTO="$ROOT/optolink"
g++ -std=c++17 -Wall -Wextra \
  -I"$ROOT" -I"$OPTO" \
  test_vs2_transaction.cpp \
  "$OPTO/constants.cpp" \
  "$OPTO/datapoint/datapoint.cpp" \
  "$OPTO/datapoint/converter.cpp" \
  "$OPTO/datapoint/conversion_helpers.cpp" \
  "$OPTO/protocol/vs2/vs2.cpp" \
  "$OPTO/protocol/vs2/parser_vs2.cpp" \
  "$OPTO/protocol/vs2/packet_vs2.cpp" \
  -o vs2_transaction_harness
# Engine debug logging is compiled out on host (logging.h is gated on
# VITOHOME_DEBUG_OPTOLINK && ESP_PLATFORM), so stdout is the harness only.
./vs2_transaction_harness
