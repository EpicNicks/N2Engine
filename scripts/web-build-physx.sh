#!/usr/bin/env bash
# Builds NVIDIA PhysX for WebAssembly (stage 5 of issue #155; docs/web-build.html, "PhysX on the web").
#
#   scripts/web-build-physx.sh <commit> <out-dir> <emscripten-dir> [<build-rev>] [<compiler-version>]
#
#   <commit>          the full 40-character commit of https://github.com/NVIDIA-Omniverse/PhysX to build (the pin lives in
#                     cmake/PhysXWebPin.cmake; the tag 107.3-omni-and-physx-5.6.1 is that commit). It is fetched by hash
#                     and refused unless `git rev-parse HEAD` returns the same hash.
#   <out-dir>         where the result goes: lib/lib*.a, include/ (the SDK's headers with the two patches below),
#                     LICENSE.md and .pin (the stamp cmake/PhysXWeb.cmake compares). Replaced as a whole on success.
#   <emscripten-dir>  the directory with emcc, em++ and emcmake (the Emscripten SDK; cmake/PhysXWeb.cmake passes the
#                     directory of the em++ it is configured with)
#   <build-rev>       the number in cmake/PhysXWebPin.cmake that is bumped when this script's flags or patches change
#   <compiler-ver>    the compiler version the stamp records (CMAKE_CXX_COMPILER_VERSION)
#
# What it builds: the PhysX public CMake project (physx/compiler/public) with its Linux platform files, which is how
# the PhysX source itself supports Emscripten (the headers define PX_EMSCRIPTEN, PX_LINUX and PX_X86 for __EMSCRIPTEN__):
# CPU only (no CUDA, no GPU library), static libraries, Release, no snippets, no OmniPVD, no PVD runtime. Only the
# libraries the engine links are built: PhysX, PhysXCommon, PhysXFoundation, PhysXExtensions, PhysXPvdSDK.
#
# Flags: -DPX_SIMD_DISABLED (PhysX's scalar math path: the SSE2 path needs -msimd128, which would make the whole module
# need WebAssembly SIMD; the engine defines the same macro, see engine/CMakeLists.txt), -w, and PhysX's own -O3.
# The libraries have no exceptions and no RTTI (PhysX's flags) and no threads: the engine uses the dispatcher with 0
# workers, so no PhysX thread is ever created.
#
# Patches (applied to the checkout only, each one checked to have applied; nothing here edits this repository's
# vendored headers). They are the whole difference between the PhysX tag and a build that compiles for wasm32:
#   1. physx/source/compiler/cmake/linux/CMakeLists.txt: -Werror removed (clang-wasm warns where clang-linux does not)
#   2. include/foundation/PxMath.h: the `.symver expf/powf` glibc pins are skipped for Emscripten (not assembler syntax)
#   3. include/foundation/PxHash.h: the size_t overload of PxComputeHash (written for Apple, where size_t is unsigned
#      long) is also compiled for Emscripten, where size_t is unsigned long on wasm32 and matches no other overload
#   4. source/foundation/unix/FdUnixThread.cpp: the x86 `pause` inline assembly is dropped (WebAssembly has none)
#   5. source/foundation/FdTempAllocator.cpp: an ambiguous PxHighestSetBit(unsigned long) call gets an explicit PxU32
#   6. source/foundation/unix/FdUnixMutex.cpp: the pthread_mutexattr_setprioceiling call is dropped (Emscripten has no
#      such function; a priority ceiling means nothing without threads)
# After the build a smoke program (the one of .github/workflows/physx-linux.yml) is compiled against the result and run
# under node: a foundation, a physics SDK, a scene on a 0-worker dispatcher and a falling sphere.

set -euo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <commit> <out-dir> <emscripten-dir> [<build-rev>] [<compiler-version>]" >&2
    exit 2
fi
COMMIT="$1"
OUT="$2"
EMDIR="$3"
BUILD_REV="${4:-0}"
COMPILER_VERSION="${5:-unknown}"
PHYSX_REPO="https://github.com/NVIDIA-Omniverse/PhysX"

if ! printf '%s' "$COMMIT" | grep -Eq '^[0-9a-f]{40}$'; then
    echo "the PhysX commit must be a full 40-character lowercase hash (got '$COMMIT')" >&2
    exit 2
fi
for tool in git cmake; do
    command -v "$tool" > /dev/null || { echo "$tool is needed to build PhysX for the web" >&2; exit 2; }
done
[ -x "$EMDIR/emcmake" ] || { echo "no emcmake in $EMDIR" >&2; exit 2; }

GENERATOR=()
if command -v ninja > /dev/null; then
    GENERATOR=(-G Ninja)
fi

mkdir -p "$(dirname "$OUT")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/n2-physx-web.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
JOBS="$(nproc 2> /dev/null || echo 4)"

echo "== PhysX $COMMIT for WebAssembly (build rev $BUILD_REV, compiler $COMPILER_VERSION) =="
SRC="$WORK/physx-src"
git init -q "$SRC"
git -C "$SRC" remote add origin "$PHYSX_REPO"
git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
git -C "$SRC" checkout -q FETCH_HEAD
GOT="$(git -C "$SRC" rev-parse HEAD)"
if [ "$GOT" != "$COMMIT" ]; then
    echo "fetched $GOT, expected $COMMIT" >&2
    exit 1
fi
grep -n "PX_PHYSICS_VERSION_M" "$SRC/physx/include/foundation/PxPhysicsVersion.h"

# patch <file> <sed-expression> <text that must be in the file afterwards>
patch_file() {
    sed -i "$2" "$SRC/physx/$1"
    if ! grep -qF -- "$3" "$SRC/physx/$1"; then
        echo "patch to $1 did not apply (is the PhysX commit the pinned one?)" >&2
        exit 1
    fi
}
patch_file source/compiler/cmake/linux/CMakeLists.txt 's/ -Werror//g' 'CLANG_WARNINGS'
if grep -q -- ' -Werror' "$SRC/physx/source/compiler/cmake/linux/CMakeLists.txt"; then
    echo "-Werror is still in the PhysX Linux CMakeLists.txt" >&2
    exit 1
fi
patch_file include/foundation/PxMath.h \
    's/^#if (PX_LINUX_FAMILY && !PX_ARM_FAMILY)\r\{0,1\}$/#if (PX_LINUX_FAMILY \&\& !PX_ARM_FAMILY \&\& !PX_EMSCRIPTEN)/' \
    '#if (PX_LINUX_FAMILY && !PX_ARM_FAMILY && !PX_EMSCRIPTEN)'
patch_file include/foundation/PxHash.h \
    's/^#if PX_APPLE_FAMILY\r\{0,1\}$/#if PX_APPLE_FAMILY || PX_EMSCRIPTEN/' \
    '#if PX_APPLE_FAMILY || PX_EMSCRIPTEN'
patch_file source/foundation/unix/FdUnixThread.cpp \
    's|__asm__ __volatile__("pause");|/* WebAssembly has no pause instruction */|' \
    '/* WebAssembly has no pause instruction */'
patch_file source/foundation/FdTempAllocator.cpp \
    's/PxHighestSetBit(PxU32(size) + sizeof(Chunk) - 1)/PxHighestSetBit(PxU32(PxU32(size) + sizeof(Chunk) - 1))/' \
    'PxHighestSetBit(PxU32(PxU32(size) + sizeof(Chunk) - 1))'
patch_file source/foundation/unix/FdUnixMutex.cpp \
    's|pthread_mutexattr_setprioceiling(&attr, 0);|/* no priority ceiling in WebAssembly */|' \
    '/* no priority ceiling in WebAssembly */'

PX="$SRC/physx"
BUILD="$WORK/build"
"$EMDIR/emcmake" cmake -S "$PX/compiler/public" -B "$BUILD" "${GENERATOR[@]}" --no-warn-unused-cli \
    -DCMAKE_BUILD_TYPE=release \
    "-DCMAKE_CXX_FLAGS=-w -DPX_SIMD_DISABLED" "-DCMAKE_C_FLAGS=-w" \
    -DTARGET_BUILD_PLATFORM=linux -DPX_OUTPUT_ARCH=x86 \
    -DPHYSX_ROOT_DIR="$PX" -DPX_OUTPUT_LIB_DIR="$PX" -DPX_OUTPUT_BIN_DIR="$PX" \
    -DPX_GENERATE_STATIC_LIBRARIES=True -DPX_BUILDSNIPPETS=False -DPX_BUILDPUBLICSAMPLES=False \
    -DPX_BUILDPVDRUNTIME=False -DPX_GENERATE_GPU_PROJECTS=False -DPX_GENERATE_GPU_PROJECTS_ONLY=False

LIBS=(PhysX PhysXCommon PhysXFoundation PhysXExtensions PhysXPvdSDK)
START="$(date +%s)"
cmake --build "$BUILD" --parallel "$JOBS" --target "${LIBS[@]}"
echo "PhysX built in $(( $(date +%s) - START )) s on $JOBS jobs"

# The output directory name PhysX picks (bin/<platform>/release) depends on its CMake; find it instead of guessing
LIBDIR="$(find "$PX/bin" -type d -name release | head -n 1)"
[ -n "$LIBDIR" ] || { echo "no bin/*/release directory in the PhysX build" >&2; exit 1; }

STAGE="$WORK/stage"
mkdir -p "$STAGE/lib"
for lib in "${LIBS[@]}"; do
    file="$(ls "$LIBDIR"/lib"${lib}"_static_32.a 2> /dev/null | head -n 1 || true)"
    [ -n "$file" ] || { echo "no archive for $lib in $LIBDIR" >&2; ls -l "$LIBDIR" >&2; exit 1; }
    cp "$file" "$STAGE/lib/"
done
cp -r "$PX/include" "$STAGE/include"
cp "$SRC/LICENSE.md" "$STAGE/LICENSE.md"
ls -l "$STAGE/lib"

# Smoke test: a scene with a falling sphere, linked against the staged libraries and run under node
mkdir -p "$WORK/smoke"
cat > "$WORK/smoke/main.cpp" <<'CPP'
#include <PxPhysicsAPI.h>
#include <cstdio>
using namespace physx;
int main() {
    static PxDefaultAllocator alloc;
    static PxDefaultErrorCallback err;
    PxFoundation* f = PxCreateFoundation(PX_PHYSICS_VERSION, alloc, err);
    if (!f) { std::puts("PxCreateFoundation failed"); return 1; }
    PxPhysics* p = PxCreatePhysics(PX_PHYSICS_VERSION, *f, PxTolerancesScale(), false, nullptr);
    if (!p) { std::puts("PxCreatePhysics failed"); return 2; }
    PxSceneDesc d(p->getTolerancesScale());
    d.gravity = PxVec3(0, -9.81f, 0);
    PxDefaultCpuDispatcher* cpu = PxDefaultCpuDispatcherCreate(0);
    d.cpuDispatcher = cpu;
    d.filterShader = PxDefaultSimulationFilterShader;
    PxScene* s = p->createScene(d);
    PxMaterial* m = p->createMaterial(0.5f, 0.5f, 0.1f);
    PxRigidDynamic* b = p->createRigidDynamic(PxTransform(PxVec3(0, 10, 0)));
    PxRigidActorExt::createExclusiveShape(*b, PxSphereGeometry(1.0f), *m);
    s->addActor(*b);
    for (int i = 0; i < 30; ++i) { s->simulate(1.0f / 60.0f); s->fetchResults(true); }
    float y = b->getGlobalPose().p.y;
    std::printf("PhysX %d.%d.%d ok, workers %u, body y after 0.5 s: %f\n", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR,
                PX_PHYSICS_VERSION_BUGFIX, cpu->getWorkerCount(), y);
    s->release(); m->release(); cpu->release(); p->release(); f->release();
    return y < 10.0f ? 0 : 3;
}
CPP
ARCHIVES=("$STAGE"/lib/*.a)
"$EMDIR/em++" -std=c++17 -O1 -DNDEBUG -DPX_SIMD_DISABLED -isystem "$STAGE/include" "$WORK/smoke/main.cpp" \
    "${ARCHIVES[@]}" -o "$WORK/smoke/smoke.js" -sALLOW_MEMORY_GROWTH=1
NODE="$(command -v node || true)"
if [ -z "$NODE" ] && [ -d "$EMDIR/../../node" ]; then
    NODE="$(ls "$EMDIR"/../../node/*/bin/node 2> /dev/null | head -n 1 || true)"
fi
[ -n "$NODE" ] || { echo "node is needed to run the smoke test" >&2; exit 1; }
"$NODE" "$WORK/smoke/smoke.js"

printf 'commit=%s\nbuild-rev=%s\ncompiler=%s\n' "$COMMIT" "$BUILD_REV" "$COMPILER_VERSION" > "$STAGE/.pin"
rm -rf "$OUT"
mv "$STAGE" "$OUT"
echo "PhysX for WebAssembly is in $OUT"
