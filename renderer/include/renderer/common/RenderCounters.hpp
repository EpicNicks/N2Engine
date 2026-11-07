#pragma once

#include <cstdint>

namespace Renderer::Common
{
    /**
     * What one frame drew (EN5, the editor's Profiler): counted by each backend where it records a draw, between BeginFrame
     * and EndFrame (IRenderer::GetLastFrameCounters). Every built-in pipeline draws indexed triangle lists, so a draw of n
     * indices is n / 3 triangles; a wireframe draw counts the same.
     */
    struct RenderCounters
    {
        /// Draws that reached the backend's draw call (a draw a backend skips, such as a range outside the mesh, a mesh
        /// without indices or a foreign material, is not counted)
        std::uint32_t drawCalls = 0;
        /// Indices drawn divided by 3, summed over the draws
        std::uint64_t triangles = 0;
        /// The vertex count of each drawn mesh, summed over the draws (a draw of part of a mesh counts the whole mesh's
        /// vertices: they are all bound)
        std::uint64_t vertices = 0;
        /// How many frames this renderer has finished (EndFrame) since it was made; the frame these counters are of is
        /// the newest of them
        std::uint64_t frames = 0;
    };
}
