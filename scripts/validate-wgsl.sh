#!/usr/bin/env bash
# Validates the WebGPU shaders (renderer/shaders/webgpu/*.wgsl) with Tint and naga, the WGSL compilers of Chrome/Edge
# (Dawn) and Firefox (wgpu); Safari's own WGSL compiler (WebKit) has no CI path (a check by hand). The "wgsl (Tint, naga)" CI
# job runs it; scripts/install-wgsl-validators.sh installs the pinned tools.
# Locally: TINT=/path/to/tint NAGA=/path/to/naga scripts/validate-wgsl.sh
#
# What counts as a failure:
#   - a non-zero exit of either tool, or any line starting with "warning" in its output (warnings are errors);
#   - Tint: parse, resolve and the WGSL rules including uniformity analysis (a derivative-taking textureSample or fwidth in
#     non-uniform control flow is an error), then the SPIR-V writer and spirv-val on its output (--validate), then a WGSL
#     round trip;
#   - naga: parse and validate, then the SPIR-V, MSL, HLSL and WGSL backends (what Firefox runs per platform).
# And, so the job can fail at all: every file under scripts/wgsl-negative/ must be rejected (a type error by both tools; the
# non-uniform sample by Tint, which treats it as an error; naga's analysis lets it through, so naga is not asked).
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tint="${TINT:-tint}"
naga="${NAGA:-naga}"
for tool in "$tint" "$naga"; do
  if ! command -v "$tool" > /dev/null 2>&1; then
    echo "::error::validator not found: $tool (scripts/install-wgsl-validators.sh installs it)"
    exit 2
  fi
done
echo "tint: $(command -v "$tint")"
echo "naga: $(command -v "$naga")"

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
failures=0

# run <label> <command...>: runs it, shows its output, and fails on a non-zero exit or on a line starting with "warning"
run() {
  local label="$1"
  shift
  local log status
  log="$("$@" 2>&1)"
  status=$?
  if [ -n "$log" ]; then
    printf '%s\n' "$log" | sed 's/^/    /'
  fi
  if [ "$status" -ne 0 ]; then
    echo "::error::$label failed (exit $status)"
    failures=$((failures + 1))
    return 1
  fi
  if printf '%s\n' "$log" | grep -qiE '^[[:space:]]*(warning|.*: warning)'; then
    echo "::error::$label printed a warning (warnings are errors)"
    failures=$((failures + 1))
    return 1
  fi
  echo "  ok: $label"
}

shopt -s nullglob
shaders=("$root"/renderer/shaders/webgpu/*.wgsl)
if [ "${#shaders[@]}" -ne 3 ]; then
  echo "::error::expected unlit.wgsl, lit.wgsl and text.wgsl under renderer/shaders/webgpu, found ${#shaders[@]} files"
  exit 2
fi

for shader in "${shaders[@]}"; do
  name="$(basename "$shader" .wgsl)"
  echo "== $name.wgsl"
  # Tint writes one entry point per output file (--ep), so each of the two goes through the SPIR-V writer and spirv-val
  for entry in vs_main fs_main; do
    run "tint $name.wgsl $entry (resolve, uniformity, SPIR-V + spirv-val)" "$tint" --format spvasm --validate --ep "$entry" -o "$out/$name.$entry.spvasm" "$shader"
  done
  run "tint $name.wgsl (WGSL round trip)" "$tint" --format wgsl -o "$out/$name.roundtrip.wgsl" "$shader"
  run "naga $name.wgsl (parse and validate)" "$naga" "$shader"
  # naga picks the backend from the output extension (and only prints a message, exit 0, for one it does not know), so
  # each output file must exist and not be empty
  for target in spv metal hlsl wgsl; do
    run "naga $name.wgsl -> $target" "$naga" "$shader" "$out/$name.naga.$target"
    if [ ! -s "$out/$name.naga.$target" ]; then
      echo "::error::naga wrote no $target output for $name.wgsl"
      failures=$((failures + 1))
    fi
  done
done

# The negative controls
echo "== negative controls (must be rejected)"
reject() {
  local label="$1" reason="$2"
  shift 2
  if "$@" > "$out/negative.log" 2>&1; then
    echo "::error::$label ACCEPTED an invalid shader: the validation job could not fail"
    sed 's/^/    /' "$out/negative.log"
    failures=$((failures + 1))
  elif ! grep -qiE "$reason" "$out/negative.log"; then
    # rejected, but not for the reason the control exists for (a bad flag, a writer error): it proves nothing
    echo "::error::$label rejected the shader, but its message does not match /$reason/"
    sed 's/^/    /' "$out/negative.log"
    failures=$((failures + 1))
  else
    echo "  ok: $label rejected it:"
    head -n 3 "$out/negative.log" | sed 's/^/    /'
  fi
}
negative="$root/scripts/wgsl-negative"
reject "tint (type error)" "error" "$tint" --format spvasm --validate --ep fs_main -o "$out/negative.spvasm" "$negative/type-error.wgsl"
reject "naga (type error)" "error" "$naga" "$negative/type-error.wgsl"
reject "tint (non-uniform sample)" "uniform" "$tint" --format spvasm --validate --ep fs_main -o "$out/negative.spvasm" "$negative/non-uniform-sample.wgsl"

if [ "$failures" -ne 0 ]; then
  echo "::error::$failures WGSL validation step(s) failed"
  exit 1
fi
echo "All WGSL validation steps passed"
