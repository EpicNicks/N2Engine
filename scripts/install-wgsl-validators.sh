#!/usr/bin/env bash
# Installs the two WGSL validators the "wgsl (Tint, naga)" CI job runs (scripts/validate-wgsl.sh), pinned and hash-checked:
#
#   Tint  the WGSL compiler of Chrome and Edge (Dawn): the `bin/tint` of Dawn's own prebuilt release archive
#         (https://github.com/google/dawn/releases, the ubuntu-latest Release build), the archive's SHA-256 pinned below.
#         No build: a 36 MB download. BSD-3-Clause (Copyright The Dawn & Tint Authors).
#   naga  the WGSL compiler of Firefox's wgpu: naga-cli built from its crates.io source package with `cargo install --locked`
#         (the package's own Cargo.lock pins every dependency with a checksum), the package's SHA-256 pinned below. Built here
#         because naga publishes no binaries. MIT or Apache-2.0.
#
# Both are CI tools only: nothing of them is linked into or shipped with the engine (THIRD_PARTY_NOTICES.md, "Not shipped").
#
# Usage: scripts/install-wgsl-validators.sh <directory>
# Leaves <directory>/tint/bin/tint and <directory>/naga/bin/naga. A part whose pin stamp already matches is not fetched again,
# so the directory can be cached by CI (keyed on the hash of this file, so a changed pin is a new cache).
#
# To bump a pin: change the values below, and take the hash from the file you pinned (sha256sum), not from a web page.
set -euo pipefail

# Dawn release v20261016.190333 (2026-10-17), commit 67d4d6e64b0755d638f76549794692badfdbc8ab
DAWN_TAG="v20261016.190333"
DAWN_COMMIT="67d4d6e64b0755d638f76549794692badfdbc8ab"
DAWN_ARCHIVE="Dawn-${DAWN_COMMIT}-ubuntu-latest-Release.tar.gz"
DAWN_SHA256="20ff88448def9645ceeff30911e07295aeb433be23e26180c96019b4300bff5a"

# naga-cli 30.0.1 (wgpu v30.0.1, 2026-08-22)
NAGA_VERSION="30.0.1"
NAGA_CRATE_SHA256="45283c11b8b1da1936214eefc9caf9c1bc74ae5c090023eaea84ba317a194c8c"

dest="${1:?usage: install-wgsl-validators.sh <directory>}"
mkdir -p "$dest"
dest="$(cd "$dest" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

check_sha256() {
  local expected="$1" file="$2"
  local actual
  actual="$(sha256sum "$file" | cut -d' ' -f1)"
  if [ "$actual" != "$expected" ]; then
    echo "::error::SHA-256 of $(basename "$file") is $actual, expected $expected"
    exit 1
  fi
  echo "SHA-256 ok: $(basename "$file") $actual"
}

# ---- Tint ----------------------------------------------------------------------------------------------------------
tint_stamp="dawn $DAWN_TAG $DAWN_COMMIT $DAWN_SHA256"
if [ -x "$dest/tint/bin/tint" ] && [ "$(cat "$dest/tint/.pin" 2>/dev/null)" = "$tint_stamp" ]; then
  echo "Tint: pin matches, already installed"
else
  rm -rf "$dest/tint"
  curl --fail --silent --show-error --location --retry 5 --retry-delay 5 \
    -o "$work/$DAWN_ARCHIVE" "https://github.com/google/dawn/releases/download/${DAWN_TAG}/${DAWN_ARCHIVE}"
  check_sha256 "$DAWN_SHA256" "$work/$DAWN_ARCHIVE"
  mkdir -p "$dest/tint"
  tar -xzf "$work/$DAWN_ARCHIVE" -C "$dest/tint" --strip-components=1 "Dawn-${DAWN_COMMIT}-ubuntu-latest-Release/bin/tint"
  test -x "$dest/tint/bin/tint"
  echo "$tint_stamp" > "$dest/tint/.pin"
  echo "Tint: installed from Dawn $DAWN_TAG"
fi

# ---- naga ----------------------------------------------------------------------------------------------------------
naga_stamp="naga-cli $NAGA_VERSION $NAGA_CRATE_SHA256"
if [ -x "$dest/naga/bin/naga" ] && [ "$(cat "$dest/naga/.pin" 2>/dev/null)" = "$naga_stamp" ]; then
  echo "naga: pin matches, already installed"
else
  rm -rf "$dest/naga"
  curl --fail --silent --show-error --location --retry 5 --retry-delay 5 \
    -o "$work/naga-cli.crate" "https://static.crates.io/crates/naga-cli/naga-cli-${NAGA_VERSION}.crate"
  check_sha256 "$NAGA_CRATE_SHA256" "$work/naga-cli.crate"
  mkdir -p "$work/src"
  tar -xzf "$work/naga-cli.crate" -C "$work/src"
  # --locked: the Cargo.lock inside the package decides every dependency version (and checks its checksum)
  cargo install --locked --path "$work/src/naga-cli-${NAGA_VERSION}" --root "$dest/naga"
  test -x "$dest/naga/bin/naga"
  echo "$naga_stamp" > "$dest/naga/.pin"
  echo "naga: built from naga-cli $NAGA_VERSION"
fi
