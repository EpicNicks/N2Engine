Accepted by the user 2026-10-15:
- Lighting uses a CPU copy plus a per-ring-chunk copy at the first lit draw after each update (frame set binding 1 is UNIFORM_BUFFER_DYNAMIC); the stage B single lighting buffer is removed
- Lit cross diff: mean is over the per-pixel LARGEST channel difference (stricter reading); coverage edges come from 'pixel != clear colour' (not colour differences); exclusion cap 25% of the image; the old share-only mode is now refused
- Vulkan lit mirrors set uHasTexture and the uHas*Texture flags through the dual-material helper as Material::ApplyUniforms does (software infers them); renderer does not infer them, defaults 1x1 textures kept unchanged and shown neutral
- SoftwareRenderer does not clamp directional lights to 2: the five-directional-light clamp test is Vulkan-only (no cross diff)
- Added test hooks GetLightingBlockForTest, GetMaterialBlockForTest, SetMaterialTextureIdForTest (stale-id injection instead of forcing allocator address reuse; the 64-try real reuse loop stays as an extra)
- Model/EditMode goldens live in tests/renderer/vulkan (renderer_tests links engine); tests/common include dir added to renderer_tests
- No per-scene tolerance loosened: all 115 lit comparisons had mean <= 0.033/255, max 1/255 on lavapipe
- Review fix: lit coverage edges now come from flat unlit mask renders (Options::coverageA/B; the model golden uses each frame as its own mask as its materials are unlit); without masks the image is its own mask (harness tests only)
- Review LOW fix: testing.html and PR body now describe the mask-based coverage rule; stale pre-mask exclusion numbers dropped, not re-measured (CI logs do not print them)
