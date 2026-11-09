#!/usr/bin/env bash
# Checks that external/box2d is exactly what external/box2d/SHA256SUMS says (the files of Box2D v3.2.0 as upstream ships
# them, see external/box2d/VENDORED.md): every file's SHA-256 matches and no other file exists under the vendored folders.
# Prints one line and exits 0 when it is so; exits 1 with the differences otherwise.
# Usage: scripts/check-box2d-vendor.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/external/box2d"
cd "$root"

sha256sum --quiet -c SHA256SUMS

listed="$(mktemp)"
present="$(mktemp)"
trap 'rm -f "$listed" "$present"' EXIT
sed -e 's/^[0-9a-f]* [ *]//' SHA256SUMS | LC_ALL=C sort > "$listed"
find src include shared LICENSE -type f | LC_ALL=C sort > "$present"
if ! diff "$listed" "$present"; then
  echo "external/box2d has files that SHA256SUMS does not list (or the reverse)" >&2
  exit 1
fi
echo "external/box2d matches SHA256SUMS ($(wc -l < "$listed" | tr -d ' ') files)"
