#pragma once

// N2ENGINE_PHYSX_GPU_PROBE is 1 where the engine talks to PhysX's CUDA entry points (the GPU probe, the GPU scene and
// the PBD particle driver) and 0 everywhere else. Only Windows has a GPU library (PhysXGpu_64.dll ships in the Windows
// PhysX package); the Linux static libraries are CPU-only, so there nothing CUDA-related is ever called or compiled.
#ifdef N2ENGINE_PHYSX_ENABLED
#include <foundation/PxPreprocessor.h>
#if defined(_WIN32) && PX_SUPPORT_GPU_PHYSX
#define N2ENGINE_PHYSX_GPU_PROBE 1
#else
#define N2ENGINE_PHYSX_GPU_PROBE 0
#endif
#else
#define N2ENGINE_PHYSX_GPU_PROBE 0
#endif
