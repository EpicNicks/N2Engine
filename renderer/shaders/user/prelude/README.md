# User-shader prelude templates (contract v1)

Data files read by `tools/n2shader` (and, later, by a C++ twin that must produce the same text). One template per target;
the cook concatenates, in this order: the version header (`#version 450` plus the line-directive extension for the SPIR-V
targets, `#version 330 core` for OpenGL), the target template below (directives expanded), the generated Material block
and texture-slot constants, then the author's fragment stage behind a `#line` directive.

Template directives (whole lines):

- `//@include <file>`: the named file of this folder, inserted as is.
- `//@if-lighting` ... `//@end-if`: kept only when the shader declares `"uses": ["lighting"]`.
- `//@each-texture` ... `//@end-each`: repeated once per texture slot the shader declares, with `@SLOT@` (0-4),
  `@TEX_BINDING@` (set 1, binding 1 + slot) and `@SAMP_BINDING@` (set 1, binding 6 + slot, WebGPU only) replaced.

The names, offsets and bindings are the contract's; `tools/n2shader/n2shader.test.mjs` compares them with
`renderer/include/renderer/common/GpuShaderInterface.hpp`, so a drift in either fails a test.

The targets differ in four places only: the PerDraw block (`row_major` on Vulkan, which copies the engine's row-major
matrices as they are; column-major on WebGPU, whose backend transposes on upload; plain named uniforms on OpenGL), the
texture spelling (combined `sampler2D` on Vulkan and OpenGL, separate `texture2D` + `sampler` on WebGPU, the form Tint
translates with fixed bindings), `n2_FragCoord()` (OpenGL's origin is bottom-left, so it needs the target height, the
uniform `n2_TargetHeight` that the OpenGL renderer sets in stage 4), and the version header. Authors write
`n2_texture(slot, uv)`, `n2_FragCoord()` and the `n2_` names, never the spelling.

A `bool` parameter is declared as an `int` (a bool is not allowed in a uniform block), 0 or 1. A `texture` parameter is
declared as a `const int` holding its slot, to pass to `n2_texture`.

## Custom vertex stage templates (stage 9d)
`vertex-vulkan.glsl`, `vertex-webgpu.glsl` and `vertex-opengl.glsl` are the preludes of a `#stage vertex` block, each including `vertex-common.glsl` (`n2_DefaultVertex`, `n2_SetPosition`, the position flag). They are used only for a shader that has the block (`buildVertexSource` in `tools/n2shader/lib/prelude.mjs`); the C++ twin does not build vertex stages, so they are not in the engine's embedded list. The Vulkan and WebGPU texts reuse the fragment preludes' block conventions (row_major PerDraw on Vulkan, column-major on WebGPU), the clip remap `N2_CLIP_Z`, and write all five varyings at locations 0-4.

## Atlas lookups (stage 9e)
`texture-atlas.glsl` defines `n2_textureCube` and `n2_texture3D` over the prelude's `n2_textureLod` and `n2_textureSize`; the cook adds it after the prelude only for a header that declares a `cube` or `texture3d` parameter (`usesAtlasTextures`). The conventions (face order, slice order) are in the file's header comment and in `docs/user-shaders.html#atlases`.
