#pragma once

// Build-time switches of the renderer library.
//
// N2ENGINE_RENDERER_VULKAN is 1 when the Vulkan backend is compiled in and 0 when it is compiled out (the CMake option
// of the same name; the renderer target defines it for everything that links it). A translation unit that does not
// get the definition from CMake sees the default, 1, so a missing definition never silently turns the backend off.
#ifndef N2ENGINE_RENDERER_VULKAN
#define N2ENGINE_RENDERER_VULKAN 1
#endif

// N2ENGINE_RENDERER_WEBGPU is 1 when the WebGPU backend is compiled in (the CMake option of the same name: natively the CI test
// harness of #155, never selectable at run time; always on in the web build, stage G4) and 0 when it is not. The opposite default to Vulkan's: a translation unit that does
// not get the definition from CMake sees 0, so a missing definition never turns the backend on.
#ifndef N2ENGINE_RENDERER_WEBGPU
#define N2ENGINE_RENDERER_WEBGPU 0
#endif
