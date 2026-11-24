#!/usr/bin/env bash
# Validates the three built-in WGSL files, the two compute kernel files and the loader canvas shader with Tint and naga, the WGSL compilers of Chrome/Edge
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

# ---- User shaders, stage 0 (orchestrator/proposals/user-shaders.md, section 10): does this platform's Tint have a SPIR-V reader?
# The cook tool (tools/n2shader) translates glslang's SPIR-V to WGSL with `tint`; a Dawn release build could be made without the
# reader. One SPIR-V module built by glslang goes through `tint --format wgsl`: the build has the reader or this fails, with
# the stage-1 fallback named (pin another Dawn build, or the Windows/macOS archives, for the cook).
glslang="${GLSLANG:-glslangValidator}"
if ! command -v "$glslang" > /dev/null 2>&1; then
  echo "::error::glslangValidator not found: $glslang (apt install glslang-tools)"
  exit 2
fi
echo "glslang: $(command -v "$glslang")"
echo "== user shaders, stage 0: this tint has a SPIR-V reader"
if run "glslang scripts/spirv-probe/probe.frag -> SPIR-V" "$glslang" -V --target-env vulkan1.1 -S frag -o "$out/probe.spv" "$root/scripts/spirv-probe/probe.frag"; then
  if run "tint SPIR-V reader: probe.spv -> WGSL" "$tint" --format wgsl -o "$out/probe.wgsl" "$out/probe.spv" \
     && [ -s "$out/probe.wgsl" ] && grep -q '@fragment' "$out/probe.wgsl"; then
    echo "  ok: the Linux tint ($(uname -sm)) reads SPIR-V and wrote $(wc -l < "$out/probe.wgsl") lines of WGSL"
  else
    echo "::error::STAGE 0 FAILED: this Dawn tint has no usable SPIR-V reader on this platform. The user-shader cook (tools/n2shader) cannot use it here: pin a Dawn build that has the reader, or run the cook's Tint stage from the Windows/macOS Dawn archives (proposal section 10, stage 1 risk row)"
    failures=$((failures + 1))
  fi
fi

shopt -s nullglob
shaders=("$root"/renderer/shaders/webgpu/*.wgsl)
if [ "${#shaders[@]}" -ne 5 ]; then
  echo "::error::expected unlit.wgsl, lit.wgsl, text.wgsl, user.wgsl and post.wgsl under renderer/shaders/webgpu, found ${#shaders[@]} files"
  exit 2
fi
# The canvas pass lives in the loader. Validate its exact runtime source too;
# duplicating it in a .wgsl file could let the shipped shader drift untested.
if ! command -v node > /dev/null 2>&1; then
  echo "::error::node is needed to extract the canvas presentation WGSL"
  exit 2
fi
node -e 'process.stdout.write(require(process.argv[1]).presenterWgsl)' "$root/web/n2-loader.js" > "$out/present.wgsl" || exit 2
shaders+=("$out/present.wgsl")

# The compute kernels of the engine's WebGPU simulations (renderer/shaders/webgpu/compute/): every entry point, as each file
# declares them ("@compute @workgroup_size(...) fn name")
compute_shaders=("$root"/renderer/shaders/webgpu/compute/*.wgsl)
if [ "${#compute_shaders[@]}" -ne 2 ]; then
  echo "::error::expected particles.wgsl and softbody.wgsl under renderer/shaders/webgpu/compute, found ${#compute_shaders[@]} files"
  exit 2
fi
shaders+=("${compute_shaders[@]}")

for shader in "${shaders[@]}"; do
  name="$(basename "$shader" .wgsl)"
  echo "== $name.wgsl"
  # Tint writes one entry point per output file (--ep), so each of the two goes through the SPIR-V writer and spirv-val
  entries=(vs_main fs_main)
  if [ "$name" = present ]; then entries=(vs fs); fi
  # user.wgsl is the vertex stage of the user shaders only: their fragment stage is the cooked WGSL, checked by the corpus below
  if [ "$name" = user ]; then entries=(vs_main); fi
  case "$shader" in
    */compute/*)
      mapfile -t entries < <(grep -oE '@compute[^;{]*fn [A-Za-z_][A-Za-z0-9_]*' "$shader" | awk '{print $NF}')
      if [ "${#entries[@]}" -eq 0 ]; then
        echo "::error::$name.wgsl declares no compute entry point"
        failures=$((failures + 1))
      fi
      ;;
  esac
  for entry in "${entries[@]}"; do
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

# The user-shader conformance corpus (renderer/shaders/user/corpus, tools/n2shader): every positive file through glslang ->
# spirv-val -> the reflection self-check -> Tint -> naga, and every negative rejected at the stage and for the reason in its
# expect.json entry (--require-tools: a missing tool fails here instead of skipping a stage)
echo "== user shaders: the conformance corpus"
if ! command -v node > /dev/null 2>&1; then
  echo "::error::node is needed to run the user-shader cook (tools/n2shader)"
  failures=$((failures + 1))
else
  corpus_log="$out/corpus.log"
  if TINT="$tint" NAGA="$naga" GLSLANG="$glslang" node "$root/tools/n2shader/n2shader.mjs" corpus "$root/renderer/shaders/user/corpus" --require-tools > "$corpus_log" 2>&1; then
    sed 's/^/    /' "$corpus_log"
  else
    sed 's/^/    /' "$corpus_log"
    echo "::error::the user-shader corpus failed"
    failures=$((failures + 1))
  fi
fi

if [ "$failures" -ne 0 ]; then
  echo "::error::$failures WGSL validation step(s) failed"
  exit 1
fi
echo "All WGSL validation steps passed"
