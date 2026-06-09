#!/usr/bin/env bash
# Host build + run of the VS2 transaction harness.
# VW = path to the vendored VitoWiFi sources. Pre-cleanup that is the upstream
# tree; post-§1a (platform ctors removed) drop LinuxSerialInterface.cpp below.
set -euo pipefail
VW="${1:-./components/vitohome/vitowifi/src}"
g++ -std=c++17 -Wall \
  -I"$VW" -I"$VW/VS2" -I"$VW/Datapoint" -I"$VW/Interface" \
  test_vs2_transaction.cpp \
  "$VW/Constants.cpp" \
  "$VW/Datapoint/Datapoint.cpp" "$VW/Datapoint/Converter.cpp" "$VW/Datapoint/ConversionHelpers.cpp" \
  "$VW/VS2/VS2.cpp" "$VW/VS2/ParserVS2.cpp" "$VW/VS2/PacketVS2.cpp" \
  "$VW/Interface/LinuxSerialInterface.cpp" \
  -o vs2_transaction_harness
# engine PC-logging goes to stdout until the §1b Logging.h fix lands; filter it
./vs2_transaction_harness 2>/dev/null | grep -vE '^\[I\]'
