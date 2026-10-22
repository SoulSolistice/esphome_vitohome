#!/usr/bin/env bash
# Gate: basic checks over the contributed configs in example/user_curated/.
#
# WHAT IS CHECKED, PER example/user_curated/<device>/
# ---------------------------------------------------
#   * <device>.vitohome.yaml exists and is a standalone config (top-level
#     `esphome:`); every other YAML in the directory is a package fragment
#     (no `esphome:`) named *.dp.*.yaml -- the same convention as
#     validate_examples.sh.
#   * the device file defines `vitohome_repo` and `vitohome_ref`, and every
#     git source in it (`url:`) is exactly `${vitohome_repo}`. No other
#     source -- no third-party URL, no `github://` shorthand, no `type: local`
#     -- in the device file or the fragments. Beyond making the override
#     sound, this keeps a contributed config from pulling code that was never
#     reviewed in this repo into every user who copies it.
#   * credential-shaped keys (password / ssid / key) take `!secret`, never a
#     literal: a contributor's real Wi-Fi password or API key must not land in
#     a public repo.
#   * `esphome config` passes against this checkout (secrets from
#     example/secrets.example.yaml, whose placeholders are valid on purpose).
#
# NOT checked: compilation (lambdas in a curated config are only syntax-checked
# by a compile), and anything about the device itself -- addresses, scaling and
# "it works on my unit" stay the contributor's claim.
#
# Usage: bash scripts/validate_user_curated.sh   (from the repo root)
set -euo pipefail

cd "$(dirname "$0")/.."
CUR="example/user_curated"
SECRETS_SRC="example/secrets.example.yaml"

# file:// URL of the checkout. On Git Bash `pwd -W` gives E:/x (needs three
# slashes to keep the drive out of the URL's host part); elsewhere pwd is
# already absolute (/home/...).
root="$(pwd -W 2>/dev/null || pwd)"
case "$root" in
  /*) repo_url="file://$root" ;;
  *) repo_url="file:///$root" ;;
esac

head_sha="$(git rev-parse HEAD)"
# Unique per commit: ESPHome caches clones by (url, ref), so reusing one branch
# name across commits could validate a stale clone under `refresh: 1d`.
ref="ci-user-curated-${head_sha:0:12}"
git branch -f "$ref" HEAD >/dev/null

# Files this run creates next to the configs: a temporary secrets.yaml, and
# the .gitignore ESPHome drops into every config directory it reads. Removed
# on exit so a local run leaves the tree as it found it.
created_files=()
cleanup() {
  git branch -D "$ref" >/dev/null 2>&1 || true
  for f in "${created_files[@]}"; do rm -f "$f"; done
}
trap cleanup EXIT

if ! git diff --quiet HEAD -- components "$CUR"; then
  echo "WARNING: uncommitted changes under components/ or $CUR/ are NOT validated --"
  echo "         ESPHome clones the committed HEAD. Commit (or stash) them first."
fi
echo "source under test: $repo_url @ $ref ($head_sha)"

devices=0
failures=0

fail() {
  echo "FAIL  $1"
  shift
  for line in "$@"; do echo "    $line"; done
  failures=$((failures + 1))
}

for d in "$CUR"/*/; do
  d="${d%/}"
  name="$(basename "$d")"
  dev="$d/$name.vitohome.yaml"
  devices=$((devices + 1))
  ok=1

  # --- layout --------------------------------------------------------------
  if [ ! -f "$dev" ]; then
    fail "$name" "missing $name.vitohome.yaml: each $CUR/<device>/ needs a device" \
      "config named after its directory."
    continue
  fi
  if ! grep -qE '^esphome:' "$dev"; then
    fail "$name" "$name.vitohome.yaml has no top-level 'esphome:' block; the device" \
      "file must be a standalone config."
    ok=0
  fi

  # --- sources: everything through ${vitohome_repo} / ${vitohome_ref} ------
  for key in vitohome_repo vitohome_ref; do
    if ! grep -qE "^[[:space:]]+$key:" "$dev"; then
      fail "$name" "no '$key' substitution: the device file must take its sources" \
        "from \${vitohome_repo} @ \${vitohome_ref} so CI can point them at the PR."
      ok=0
    fi
  done
  while IFS= read -r line; do
    value="$(printf '%s' "$line" | sed -E 's/^[^:]*url:[[:space:]]*//; s/[[:space:]]+#.*$//; s/^["'\'']//; s/["'\'']$//')"
    # The literal text ${vitohome_repo}, unexpanded: the file must reference
    # the substitution, not a URL that merely happens to be equal to it.
    # shellcheck disable=SC2016
    if [ "$value" != '${vitohome_repo}' ]; then
      fail "$name" "source '$value' in $name.vitohome.yaml: curated configs may only" \
        "pull from this repo, written as 'url: \${vitohome_repo}'."
      ok=0
    fi
  done < <(grep -E '^[[:space:]]*(-[[:space:]]*)?url:' "$dev" || true)
  # A literal repo URL anywhere but the vitohome_repo definition would bypass
  # the override; shorthand / local sources are never allowed.
  if grep -nE 'github\.com/SoulSolistice/esphome_vitohome|SoulSolistice/esphome_vitohome' "$dev" \
    | grep -vqE '^[0-9]+:[[:space:]]+vitohome_repo:'; then
    fail "$name" "the repo URL appears outside the 'vitohome_repo' substitution;" \
      "reference it as \${vitohome_repo} so CI validates the PR, not GitHub."
    ok=0
  fi
  for f in "$d"/*.yaml; do
    base="$(basename "$f")"
    [ "$base" = secrets.yaml ] && continue
    if grep -nE 'github://|gitlab://|type:[[:space:]]*local' "$f" >/dev/null; then
      fail "$name" "$base uses a shorthand or local source; not allowed in a curated config."
      ok=0
    fi
    case "$base" in
      *.dp.*.yaml)
        if grep -qE '^[[:space:]]*(-[[:space:]]*)?url:|^external_components:' "$f"; then
          fail "$name" "$base pulls sources itself; package fragments must not --" \
            "sources belong in $name.vitohome.yaml."
          ok=0
        fi
        ;;
    esac
    # --- credentials ---------------------------------------------------------
    # A key with an inline value that is not !secret. Block-valued keys (value
    # on the following lines) are not credentials and are skipped.
    if bad="$(grep -nE '^[[:space:]]*(password|ssid|key|ota_password|api_password):[[:space:]]*[^[:space:]#]' "$f" \
      | grep -vE ':[[:space:]]*!secret[[:space:]]' | grep -vE ':[[:space:]]*!secret$')"; then
      fail "$name" "$base has a literal credential; use !secret (see $SECRETS_SRC):" \
        "$(printf '%s' "$bad" | sed -E 's/:[[:space:]]*[^:]*$/: <redacted>/')"
      ok=0
    fi
  done

  [ "$ok" = 1 ] || continue

  # --- esphome config against this checkout ---------------------------------
  if [ ! -f "$d/secrets.yaml" ]; then
    cp "$SECRETS_SRC" "$d/secrets.yaml"
    created_files+=("$d/secrets.yaml")
  fi
  [ -e "$d/.gitignore" ] || created_files+=("$d/.gitignore")
  if out="$(esphome -s vitohome_repo "$repo_url" -s vitohome_ref "$ref" config "$dev" 2>&1)"; then
    echo "PASS  $name"
  else
    fail "$name" "esphome config failed:"
    printf '%s\n' "$out" | sed 's/^/    /'
  fi
done

echo "---"
echo "curated devices: $devices   failures: $failures"

if [ "$devices" -eq 0 ]; then
  echo "ERROR: no device directories found in $CUR/ -- the gate matched nothing, which"
  echo "       almost certainly means the glob broke rather than that the configs"
  echo "       vanished. Failing rather than reporting a vacuous pass."
  exit 1
fi

[ "$failures" -eq 0 ] || exit 1
echo "user-curated configs OK"
