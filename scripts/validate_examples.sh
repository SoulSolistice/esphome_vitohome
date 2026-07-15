#!/usr/bin/env bash
# Gate: `esphome config` over the STANDALONE configs in example/.
#
# WHY THIS EXISTS
# ---------------
# ci.yml validates tests/${matrix.config} only; example/ was never gated. The
# published examples are what a user actually flashes, so a broken one is a
# broken first experience, and nothing was catching it.
#
# WHY IT IS NOT JUST `esphome config example/*.yaml`
# --------------------------------------------------
# example/ deliberately mixes two kinds of file:
#
#   STANDALONE configs  -- have a top-level `esphome:` block; `esphome config`
#                          is meaningful.
#   PACKAGE fragments   -- `*.dp.*.yaml` (datapoint packages) and everything in
#                          example/catalogs/. These are consumed via `packages:`
#                          or `!include` (see example/README.md and
#                          example/vscotho1_72.vitohome.yaml). They have NO
#                          top-level `esphome:` block BY CONSTRUCTION and can
#                          never pass a standalone validate.
#
# A naive `for f in example/*.yaml` sweep reports the fragments as failures
# ("'esphome' section missing from configuration"). That is a property of the
# sweep, not a defect in the file -- and reading it as a defect is exactly the
# false positive this script exists to prevent from recurring.
#
# THE CONVENTION IS CHECKED, NOT ASSUMED
# --------------------------------------
# Rather than hardcode a skip list that silently rots, this script derives the
# classification from the file's CONTENT (does it have a top-level `esphome:`?)
# and then asserts the NAMING CONVENTION agrees with it, in both directions:
#
#   * a fragment (no `esphome:`) MUST be named *.dp.*.yaml
#   * a *.dp.*.yaml file MUST NOT have an `esphome:` block
#
# So a new standalone example is picked up and validated automatically, while a
# fragment named wrong -- or a standalone accidentally given the .dp. infix --
# fails loudly here instead of being quietly skipped.
#
# Top-level detection is `grep -E '^esphome:'`: a key at column 0 is top-level
# by YAML's own rules. A real YAML parse is not an option -- these files carry
# ESPHome-specific tags (!secret, !include) that a plain PyYAML load rejects.
#
# example/catalogs/ is excluded wholesale: every file in it is a generated
# catalog package by definition (scripts/gen_catalog.py output). Validating
# them standalone is meaningless; they are exercised through the configs that
# include them.
#
# Usage: bash scripts/validate_examples.sh   (from the repo root)
set -euo pipefail

cd "$(dirname "$0")/.."
EX="example"

# `esphome config` resolves !secret against secrets.yaml next to the config.
# secrets.example.yaml ships placeholders chosen to be syntactically valid
# precisely so an unedited copy validates (the API key is base64 of 32 zero
# bytes -- valid, and PUBLIC; see the file's own header). Only create it if the
# user has not already put a real one there.
if [ ! -f "$EX/secrets.yaml" ]; then
  cp "$EX/secrets.example.yaml" "$EX/secrets.yaml"
  CREATED_SECRETS=1
  trap 'rm -f "$EX/secrets.yaml"' EXIT
else
  CREATED_SECRETS=0
fi
echo "secrets.yaml: $([ "$CREATED_SECRETS" = 1 ] && echo 'created from secrets.example.yaml (temporary)' || echo 'pre-existing, left alone')"

standalone=0
fragments=0
failures=0

for f in "$EX"/*.yaml; do
  base="$(basename "$f")"
  case "$base" in
    secrets.example.yaml|secrets.yaml) continue ;;
  esac

  if grep -qE '^esphome:' "$f"; then
    # --- standalone: must NOT carry the fragment naming infix -------------
    case "$base" in
      *.dp.*.yaml)
        echo "CONVENTION FAIL  $base"
        echo "    has a top-level 'esphome:' block but is named '*.dp.*.yaml',"
        echo "    which this repo reserves for datapoint PACKAGE fragments."
        echo "    Rename it, or drop the esphome: block if it is meant to be a package."
        failures=$((failures + 1))
        continue
        ;;
    esac
    standalone=$((standalone + 1))
    if out="$(esphome config "$f" 2>&1)"; then
      echo "PASS  $base"
    else
      echo "FAIL  $base"
      printf '%s\n' "$out" | sed 's/^/    /'
      failures=$((failures + 1))
    fi
  else
    # --- fragment: must carry the naming infix ---------------------------
    case "$base" in
      *.dp.*.yaml)
        fragments=$((fragments + 1))
        echo "SKIP  $base  (package fragment: no top-level esphome:, consumed via packages:/!include)"
        ;;
      *)
        echo "CONVENTION FAIL  $base"
        echo "    has no top-level 'esphome:' block, so it is a package fragment,"
        echo "    but it is not named '*.dp.*.yaml'. Rename it so the gate can"
        echo "    tell it apart from a standalone config, or add an esphome: block."
        failures=$((failures + 1))
        ;;
    esac
  fi
done

echo "---"
echo "standalone validated: $standalone   fragments skipped: $fragments   failures: $failures"

if [ "$standalone" -eq 0 ]; then
  echo "ERROR: no standalone configs found in $EX/ -- the gate matched nothing, which"
  echo "       almost certainly means the glob or the detection broke rather than that"
  echo "       the examples vanished. Failing rather than reporting a vacuous pass."
  exit 1
fi

[ "$failures" -eq 0 ] || exit 1
echo "example configs OK"
