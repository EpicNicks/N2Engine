#!/usr/bin/env bash
# Checks that external/JoltPhysics is exactly what external/JoltPhysics/SHA256SUMS says (the files of Jolt Physics
# v5.6.0 and its LICENSE as upstream ships them, see external/JoltPhysics/VENDORED.md): every file's SHA-256 matches and
# no other file exists under Jolt/. Prints one line and exits 0 when it is so; exits 1 with the differences otherwise.
# Usage: scripts/check-jolt-vendor.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/external/JoltPhysics"
cd "$root"

sha256sum --quiet -c SHA256SUMS

listed="$(mktemp)"
present="$(mktemp)"
trap 'rm -f "$listed" "$present"' EXIT
sed -e 's/^[0-9a-f]* [ *]//' SHA256SUMS | LC_ALL=C sort > "$listed"
{ find Jolt -type f; echo LICENSE; } | LC_ALL=C sort > "$present"
if ! diff "$listed" "$present"; then
  echo "external/JoltPhysics has files that SHA256SUMS does not list (or the reverse)" >&2
  exit 1
fi
echo "external/JoltPhysics matches SHA256SUMS ($(wc -l < "$listed" | tr -d ' ') files)"
