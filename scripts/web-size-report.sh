#!/usr/bin/env bash
# Reports the sizes of the built web artifacts (.wasm and .js; also .data, the preloaded asset pack, and .html, the page,
# which a web package has), raw and gzip -9, as a table on stdout and, when
# GITHUB_STEP_SUMMARY is set, in the job summary. Fails only when a budget is configured and exceeded:
#   N2ENGINE_WEB_BUDGET_WASM_GZIP   gzip bytes allowed for any one .wasm file
#   N2ENGINE_WEB_BUDGET_TOTAL_GZIP  gzip bytes allowed for all .wasm and .js files together (.data and .html are listed,
#                                   not counted)
# Unset or empty means no budget (the stage 0 default). Brotli sizes are not reported yet.
# Usage: scripts/web-size-report.sh [directory]   (default: build/bin)
set -euo pipefail

dir="${1:-build/bin}"
wasm_budget="${N2ENGINE_WEB_BUDGET_WASM_GZIP:-}"
total_budget="${N2ENGINE_WEB_BUDGET_TOTAL_GZIP:-}"

for v in "$wasm_budget" "$total_budget"; do
  case "$v" in
    ''|*[!0-9]*) if [ -n "$v" ]; then echo "budget '$v' is not a number of bytes" >&2; exit 2; fi ;;
  esac
done

if [ ! -d "$dir" ]; then
  echo "no such directory: $dir" >&2
  exit 2
fi

table="$(mktemp)"
trap 'rm -f "$table"' EXIT
total_gzip=0
count=0
failed=0
messages=""

while IFS= read -r f; do
  raw=$(wc -c < "$f" | tr -d ' ')
  gz=$(gzip -9 -c "$f" | wc -c | tr -d ' ')
  count=$((count + 1))
  case "$f" in
    *.wasm|*.js) total_gzip=$((total_gzip + gz)) ;;
  esac
  printf '| %s | %s | %s |\n' "${f#"$dir"/}" "$raw" "$gz" >> "$table"
  case "$f" in
    *.wasm)
      if [ -n "$wasm_budget" ] && [ "$gz" -gt "$wasm_budget" ]; then
        failed=1
        messages="${messages}${f#"$dir"/}: gzip ${gz} bytes exceeds the .wasm budget ${wasm_budget}"$'\n'
      fi
      ;;
  esac
done < <(find "$dir" -type f \( -name '*.wasm' -o -name '*.js' -o -name '*.data' -o -name '*.html' \) | LC_ALL=C sort)

if [ -n "$total_budget" ] && [ "$total_gzip" -gt "$total_budget" ]; then
  failed=1
  messages="${messages}total gzip ${total_gzip} bytes exceeds the total budget ${total_budget}"$'\n'
fi

report() {
  echo "### Web artifact sizes (${dir})"
  echo
  echo "| file | raw bytes | gzip -9 bytes |"
  echo "|---|---:|---:|"
  cat "$table"
  echo
  echo "Files: ${count}, total gzip of the .wasm and .js files: ${total_gzip} bytes."
  echo "Budgets: .wasm gzip ${wasm_budget:-none}, total gzip ${total_budget:-none}."
  if [ -n "$messages" ]; then
    echo
    echo "**Over budget:**"
    printf '%s' "$messages"
  fi
}

report
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  report >> "$GITHUB_STEP_SUMMARY"
fi

if [ "$count" -eq 0 ]; then
  echo "no .wasm or .js files under ${dir}" >&2
  exit 1
fi
exit "$failed"
