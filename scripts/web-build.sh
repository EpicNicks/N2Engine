#!/usr/bin/env bash
# Builds the web player package (index.html, index.js, index.wasm, index.data, n2-loader.js, manifest.json) and copies it
# into a folder you host, such as a website's public/ directory. See docs/web-build.html#build-script.
#
#   scripts/web-build.sh --out DIR [--profile size|speed|dev] [--docker | --from-ci [RUN_ID]] [options]
#
# Three ways to get the package:
#   (default)      build it here with emsdk (EMSDK set, or N2_EMSDK / ~/emsdk; installed there at the pinned release when
#                  missing). Linux and macOS; on Windows use --docker (PhysX for the web is built by a bash script).
#   --docker       build it in the emscripten/emsdk image of the pinned release (Docker Desktop on Windows works); the
#                  build folder stays in the repository (build-web-docker-<profile>), so a second run is incremental.
#   --from-ci      download the size-profile package that CI built (the web job's `webgpu-browser-inputs` artifact,
#                  kept 7 days) with the gh CLI: of RUN_ID, or of the latest successful tests.yml run on the current
#                  branch. Nothing is compiled; only the size profile exists there.
#
# Options:
#   --out DIR         where the package goes (required unless --no-copy). Only the package's own files are written
#                     there; anything else in DIR is left alone.
#   --profile P       size (default: the smallest download), speed, or dev (debug info, ~300 MB: not for hosting).
#   --precompressed   also copy the .br and .gz siblings (size and speed), for a host that serves them itself.
#   --smoke           run the package's node smoke test (scripts/web-package-smoke.mjs) before copying.
#   --build-dir DIR   the CMake build folder (default build-web-<profile>, or build-web-docker-<profile> with --docker).
#   --jobs N          parallel compile jobs (default: Ninja's).
#   --no-copy         build only (used by --docker inside the container).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# The Emscripten release CI builds with (one place: the web job of the workflow)
EMSCRIPTEN_VERSION="$(sed -n 's/^ *EMSCRIPTEN_VERSION: *\([0-9.]*\).*/\1/p' .github/workflows/tests.yml | head -n 1)"
[ -n "$EMSCRIPTEN_VERSION" ] || { echo "web-build: no EMSCRIPTEN_VERSION in .github/workflows/tests.yml" >&2; exit 1; }

PACKAGE_FILES=(index.html index.js index.wasm index.data n2-loader.js manifest.json)

mode=local
run_id=""
out=""
profile=size
precompressed=0
smoke=0
build_dir=""
jobs=""
copy=1

die() { echo "web-build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --out) out="${2:?--out needs a folder}"; shift 2 ;;
    --profile) profile="${2:?--profile needs size, speed or dev}"; shift 2 ;;
    --docker) mode=docker; shift ;;
    --from-ci)
      mode=ci; shift
      if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then run_id="$1"; shift; fi ;;
    --precompressed) precompressed=1; shift ;;
    --smoke) smoke=1; shift ;;
    --build-dir) build_dir="${2:?--build-dir needs a folder}"; shift 2 ;;
    --jobs) jobs="${2:?--jobs needs a number}"; shift 2 ;;
    --no-copy) copy=0; shift ;;
    -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done

case "$profile" in size|speed|dev) ;; *) die "--profile must be size, speed or dev" ;; esac
[ "$copy" = 0 ] || [ -n "$out" ] || die "--out DIR is required (or --no-copy)"
if [ "$profile" = dev ] && [ "$precompressed" = 1 ]; then die "the dev profile has no .br/.gz copies"; fi

# Copies the package in $1 to $out and checks every file against manifest.json
copy_package() {
  local from="$1" f
  for f in "${PACKAGE_FILES[@]}"; do
    [ -f "$from/$f" ] || die "$from/$f is missing: not a web package"
  done
  mkdir -p "$out"
  for f in "${PACKAGE_FILES[@]}"; do
    cp "$from/$f" "$out/$f"
    if [ "$precompressed" = 1 ] && [ "$f" != manifest.json ]; then
      [ -f "$from/$f.br" ] && [ -f "$from/$f.gz" ] || die "$from has no .br/.gz copy of $f"
      cp "$from/$f.br" "$from/$f.gz" "$out/"
    fi
  done
  # The .br/.gz copies left from an earlier --precompressed run would be stale: remove them when not copying new ones
  if [ "$precompressed" = 0 ]; then
    for f in "${PACKAGE_FILES[@]}"; do rm -f "$out/$f.br" "$out/$f.gz"; done
  fi
  if command -v node >/dev/null 2>&1; then
    (cd "$out" && node -e '
      const m = require("./manifest.json"), crypto = require("crypto"), fs = require("fs");
      let bad = 0;
      for (const [f, v] of Object.entries(m.files)) {
        const h = crypto.createHash("sha256").update(fs.readFileSync(f)).digest("hex");
        if (h !== v.sha256) { console.error("web-build: " + f + " does not match manifest.json"); bad = 1; }
      }
      console.log("profile " + m.profile + ", " + Object.keys(m.files).length + " files match manifest.json");
      process.exit(bad);')
  else
    echo "web-build: node not found, the copy was not checked against manifest.json"
  fi
  echo "web-build: the package is in $out"
}

# ------------------------------------------------------------------------------------------------ from CI
if [ "$mode" = ci ]; then
  [ "$profile" = size ] || die "--from-ci has only the size profile (CI uploads that one)"
  [ "$precompressed" = 0 ] || die "--from-ci has no .br/.gz copies (CI uploads the plain files)"
  command -v gh >/dev/null 2>&1 || die "--from-ci needs the gh CLI (https://cli.github.com), logged in"
  if [ -z "$run_id" ]; then
    branch="$(git rev-parse --abbrev-ref HEAD)"
    run_id="$(gh run list --workflow tests.yml --branch "$branch" --status success --limit 1 --json databaseId -q '.[0].databaseId')"
    [ -n "$run_id" ] || die "no successful tests.yml run on $branch: pass a RUN_ID"
  fi
  echo "web-build: run $run_id ($(gh run view "$run_id" --json headSha,headBranch -q '.headBranch + " @ " + .headSha'))"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  gh run download "$run_id" -n webgpu-browser-inputs -D "$tmp" \
    || die "run $run_id has no webgpu-browser-inputs artifact (CI keeps it 7 days)"
  copy_package "$tmp/build-web-size/web-package"
  exit 0
fi

# ------------------------------------------------------------------------------------------------ Docker
if [ "$mode" = docker ]; then
  command -v docker >/dev/null 2>&1 || die "--docker needs Docker"
  docker info >/dev/null 2>&1 || die "Docker is not running"
  build_dir="${build_dir:-build-web-docker-$profile}"
  inner=(scripts/web-build.sh --no-copy --profile "$profile" --build-dir "$build_dir")
  [ "$smoke" = 0 ] || inner+=(--smoke)
  [ -z "$jobs" ] || inner+=(--jobs "$jobs")
  # Git Bash on Windows rewrites /src-style arguments into Windows paths: not here
  src="$REPO"
  if command -v cygpath >/dev/null 2>&1; then src="$(cygpath -w "$REPO")"; fi
  echo "web-build: building in emscripten/emsdk:$EMSCRIPTEN_VERSION ($build_dir)"
  MSYS_NO_PATHCONV=1 docker run --rm -v "$src:/src" -w /src -e N2_WEB_BUILD_IN_DOCKER=1 \
    "emscripten/emsdk:$EMSCRIPTEN_VERSION" bash -c '
      set -e
      # The image'"'"'s CMake (3.22, Ubuntu'"'"'s) is older than the 3.25 the project needs: a current one from PyPI
      if ! python3 -m pip --version >/dev/null 2>&1; then
        apt-get update -qq && apt-get install -y -qq python3-pip >/dev/null
      fi
      python3 -m pip install --quiet --disable-pip-version-check "cmake>=3.25,<4" ninja
      hash -r
      cmake --version | head -n 1
      git config --global --add safe.directory "*"
      exec "$@"' bash "${inner[@]}"
  [ "$copy" = 0 ] || copy_package "$build_dir/web-package"
  exit 0
fi

# ------------------------------------------------------------------------------------------------ local emsdk
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    [ -n "${N2_WEB_BUILD_IN_DOCKER:-}" ] || die "on Windows use --docker (or --from-ci): PhysX for the web is built by a bash script with Linux tools" ;;
esac

if ! command -v emcc >/dev/null 2>&1; then
  emsdk_dir="${EMSDK:-${N2_EMSDK:-$HOME/emsdk}}"
  if [ ! -f "$emsdk_dir/emsdk_env.sh" ]; then
    echo "web-build: installing emsdk $EMSCRIPTEN_VERSION into $emsdk_dir"
    git clone --depth 1 --branch "$EMSCRIPTEN_VERSION" https://github.com/emscripten-core/emsdk.git "$emsdk_dir" \
      || git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$emsdk_dir"
  fi
  if [ ! -d "$emsdk_dir/upstream/emscripten" ] || ! grep -q "$EMSCRIPTEN_VERSION" "$emsdk_dir/.emscripten" 2>/dev/null; then
    (cd "$emsdk_dir" && ./emsdk install "$EMSCRIPTEN_VERSION" && ./emsdk activate "$EMSCRIPTEN_VERSION")
  fi
  # shellcheck disable=SC1091
  . "$emsdk_dir/emsdk_env.sh" >/dev/null
fi
have="$(emcc --version | head -n 1)"
case "$have" in
  *" $EMSCRIPTEN_VERSION "*) ;;
  *) echo "web-build: warning: $have is not Emscripten $EMSCRIPTEN_VERSION (the release CI builds and checks with)" ;;
esac
command -v ninja >/dev/null 2>&1 || die "ninja is needed (https://ninja-build.org)"

build_dir="${build_dir:-build-web-$profile}"
# The configure CI uses for the hosted profiles: Jolt and PhysX in, no tests. The first configure builds PhysX for
# WebAssembly (cmake/PhysXWeb.cmake), a couple of minutes, kept in external/PhysX/physx/bin/emscripten.wasm32.
echo "web-build: configuring $build_dir ($profile profile)"
mkdir -p "$build_dir"
emcmake cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DN2ENGINE_WEB_PROFILE="$profile" \
  -DN2ENGINE_BUILD_TESTS=OFF -DN2ENGINE_USE_JOLT=ON 2>&1 | tee "$build_dir/configure.log"
grep -q "PhysX enabled" "$build_dir/configure.log" \
  || echo "web-build: warning: PhysX is not enabled in this build (see $build_dir/configure.log)"

echo "web-build: building the web-package target"
if [ -n "$jobs" ]; then
  cmake --build "$build_dir" --target web-package -- -j "$jobs"
else
  cmake --build "$build_dir" --target web-package
fi

if [ "$smoke" = 1 ]; then
  smoke_args=("$build_dir/web-package" 30 320x180)
  [ "$profile" = dev ] || smoke_args+=(--precompressed)
  node scripts/web-package-smoke.mjs "${smoke_args[@]}"
fi

[ "$copy" = 0 ] || copy_package "$build_dir/web-package"
