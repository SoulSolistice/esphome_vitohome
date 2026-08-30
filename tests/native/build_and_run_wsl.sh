#!/usr/bin/env bash
# Windows convenience wrapper: run the host C++ suite under WSL, staged on ext4.
#
# Not a gate of its own and not called by CI -- it only stages and then invokes
# build_and_run.sh, which stays the single definition of what the suite is. On
# a Linux checkout it is redundant; run build_and_run.sh directly there.
#
# WHY STAGE. Building straight off /mnt/<drive> pays a 9p round-trip per header
# open, and this suite opens a lot of headers: 13 g++ invocations in
# build_and_run_protocols.sh alone, each pulling the same engine tree.
# Measured on one machine, same run, protocol proofs only:
#
#     /mnt/e (drvfs)   real 61.4 s   user 18.7 s
#     /tmp   (ext4)    real 37.7 s   user 17.3 s
#
# User CPU is the same to within noise -- the entire 24 s is filesystem, not
# compute. So mirror the two directories the suite actually reads onto ext4 and
# build there. Nothing is ever copied back: the Linux ELF binaries the compiles
# produce stay out of the Windows checkout, which is a side benefit rather than
# the point.
#
# USAGE, from the repo root:
#   Git Bash    wsl.exe -d <distro> --cd "$(pwd -W)" -e bash tests/native/build_and_run_wsl.sh
#   PowerShell  wsl.exe -d <distro> --cd "$PWD"      -e bash tests/native/build_and_run_wsl.sh
#
# `wsl --cd` takes the Windows path and translates it, so neither form needs a
# hand-written /mnt/... path.
#
# $VITOHOME_STAGE overrides the staging directory. Keep it STABLE across runs
# rather than pointing it at a fresh mktemp each time: a constant path is what
# lets ccache (below) hit on the second run.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="${VITOHOME_STAGE:-$HOME/.cache/vitohome-native}"

# Deliberately does NOT shim ccache into PATH. ccache only caches
# single-source `-c` compilations, and every g++ line in build_and_run.sh and
# build_and_run_protocols.sh compiles several .cpp files and links in one
# invocation, which ccache classifies as uncacheable: measured 0 of 20
# cacheable calls, 0 hits, over a full suite run. It would cost a PATH entry
# and buy nothing until those scripts compile to objects first.
case "$REPO" in
  /mnt/*)
    echo "== staging $REPO -> $STAGE =="
    mkdir -p "$STAGE/components" "$STAGE/tests"
    # Only what the suite reads. --delete so a file deleted upstream cannot
    # linger in the stage and keep passing.
    rsync -a --delete "$REPO/components/vitohome/" "$STAGE/components/vitohome/"
    rsync -a --delete "$REPO/tests/native/" "$STAGE/tests/native/"
    RUN_DIR="$STAGE/tests/native"
    ;;
  *)
    echo "== $REPO is not on /mnt -- running in place, no staging needed =="
    RUN_DIR="$REPO/tests/native"
    ;;
esac

cd "$RUN_DIR"
bash build_and_run.sh
