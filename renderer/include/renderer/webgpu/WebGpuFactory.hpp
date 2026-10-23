#pragma once

#include <memory>

#include "renderer/common/Renderer.hpp"

// Makes the WebGPU backend without naming a WebGPU type, so code outside the renderer (the engine's Window) needs no webgpu.h on
// its include path. This header declares the function unconditionally; it is defined only where the backend is compiled in
// (renderer/common/RendererConfig.hpp, N2ENGINE_RENDERER_WEBGPU), so calling it in a build without the backend fails to link. The
// engine calls it only in the web build (stage G4 of #155): natively the backend is the CI test harness and not selectable
// (design decision W8).
namespace Renderer::WebGpu
{
    /// A new, uninitialized Renderer::WebGpu::WebGpuRenderer
    std::unique_ptr<Common::IRenderer> CreateWebGpuRenderer();
}
