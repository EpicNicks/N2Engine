#!/usr/bin/env bash
# Reports the sizes of the built web artifacts (.wasm and .js; also .data, the preloaded asset pack, and .html, the page,
# which a web package has), raw and gzip -9, as a table on stdout and, when GITHUB_STEP_SUMMARY is set, in the job
# summary. It only reports. The size budgets are enforced by scripts/web-budget-check.mjs from web/budgets.json (the one
# place they live), which reads the package's manifest.json.
# Usage: scripts/web-size-report.sh [directory]   (default: build/bin)
set -euo pipefail

dir="${1:-build/bin}"

if [ ! -d "$dir" ]; then
  echo "no such directory: $dir" >&2
  exit 2
fi

table="$(mktemp)"
trap 'rm -f "$table"' EXIT
total_gzip=0
count=0

while IFS= read -r f; do
  raw=$(wc -c < "$f" | tr -d ' ')
  gz=$(gzip -9 -c "$f" | wc -c | tr -d ' ')
  count=$((count + 1))
  case "$f" in
    *.wasm|*.js) total_gzip=$((total_gzip + gz)) ;;
  esac
  printf '| %s | %s | %s |\n' "${f#"$dir"/}" "$raw" "$gz" >> "$table"
done < <(find "$dir" -type f \( -name '*.wasm' -o -name '*.js' -o -name '*.data' -o -name '*.html' \) | LC_ALL=C sort)

report() {
  echo "### Web artifact sizes (${dir})"
  echo
  echo "| file | raw bytes | gzip -9 bytes |"
  echo "|---|---:|---:|"
  cat "$table"
  echo
  echo "Files: ${count}, total gzip of the .wasm and .js files: ${total_gzip} bytes. No budget here (see scripts/web-budget-check.mjs)."
}

report
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  report >> "$GITHUB_STEP_SUMMARY"
fi

if [ "$count" -eq 0 ]; then
  echo "no .wasm or .js files under ${dir}" >&2
  exit 1
fi
