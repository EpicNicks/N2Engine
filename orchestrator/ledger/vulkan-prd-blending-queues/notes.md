Accepted by the user 2026-10-13:
- Blended lit draw in linear lighting blends encoded values on Vulkan (GL blends linear): documented, asserted as Vulkan behaviour, no sRGB target.
- Cross-diff of blended scenes uses the opaque-mask approach (Diff::Options::ignore added to CrossBackendDiff.hpp) plus composite reference; no scene needed a looser threshold.
- TEMP ZzTempDump.cpp was added only to read numbers from CI and removed again; net diff has none of it.
- Review fix: the Transparent back-to-front golden lives in VulkanMeshGoldenTests.cpp (MeshRenderer quads, which has the software/composite helpers), not VulkanWorldCanvasGoldenTests.cpp.
