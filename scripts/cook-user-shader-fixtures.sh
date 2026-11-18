#!/usr/bin/env bash
# Regenerates the cooked fixtures the renderer's user-shader tests load (tests/renderer/fixtures/user-shaders/): for each
# <name>.n2shader, <name>/manifest.json and <name>/shader.vulkan.frag.spv, cooked by tools/n2shader for the Vulkan target only.
# Run it from the repository root after editing a fixture source or the prelude (the node test in
# tools/n2shader/n2shader.test.mjs fails when a committed manifest no longer matches its source). The SPIR-V bytes depend on
# the glslang version, so they are not compared byte for byte, only through the manifest's source hash, parameters and block.
set -euo pipefail
cd "$(dirname "$0")/.."
dir=tests/renderer/fixtures/user-shaders
for source in "$dir"/*.n2shader; do
  name="$(basename "$source" .n2shader)"
  rm -rf "$dir/$name"
  node tools/n2shader/n2shader.mjs cook "$source" --targets vulkan --require-tools --out "$dir/$name"
done
