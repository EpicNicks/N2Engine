#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshDrawing.hpp"

namespace N2Engine
{
    class IRenderable;
}

namespace N2Engine::Rendering
{
    class Sprite2D;

    /// What the sprites of one frame cost (issue #445, docs/sprite2d.html#batching): counted over every Scene::Render of the frame
    struct SpriteBatchStats
    {
        /// Sprite2D components drawn (batched or alone; one that cannot be drawn is not counted)
        std::uint32_t sprites = 0;
        /// Draw calls the sprites made: one per batch, one per sprite drawn alone
        std::uint32_t drawCalls = 0;
        /// Batches of two or more sprites
        std::uint32_t batches = 0;
        /// Sprites drawn inside those batches
        std::uint32_t batchedSprites = 0;
    };

    /**
     * Draws runs of consecutive Sprite2D components of the Transparent queue that look alike as ONE draw call (issue #445 item 4,
     * docs/sprite2d.html#batching). Scene::Render hands it the sorted Transparent queue one renderable at a time; it keeps the draw
     * order exactly, because a batch is only ever a run of sprites that were next to each other in that order, and the triangles of a
     * draw are blended in the order they are given on every backend.
     *
     * A batch is one engine Mesh, rebuilt on the CPU each frame from the sprites' shape meshes with their world matrices applied and
     * their colours in the vertex colours, drawn with an identity model matrix and a white tint through the ordinary path (GpuCache,
     * IRenderer::UpdateMesh, MeshDrawing): a dynamic vertex buffer on every backend, with no backend code of its own. Sprites join the
     * current batch while they share the texture, the blend mode, the saturation, the flash and the cut-out, and have no user shader.
     * Anything else (another renderable, a sprite that differs, a full batch) ends it. A batch of one sprite is drawn by the sprite
     * itself, exactly as without batching.
     *
     * One per Scene (its GPU meshes live with it, a slot per batch of a frame, since a backend may read a mesh only when the frame
     * ends). Render-only: nothing here touches simulation state.
     */
    class SpriteBatcher
    {
    public:
        SpriteBatcher() = default;
        ~SpriteBatcher();
        SpriteBatcher(const SpriteBatcher &) = delete;
        SpriteBatcher &operator=(const SpriteBatcher &) = delete;

        /// Starts a pass of the Transparent queue on `renderer`
        void BeginPass(Renderer::Common::IRenderer &renderer, const Renderer::Common::RenderState &state);
        /// Takes `renderable` when it is a sprite that can be batched (true: it is drawn by a later Flush, or not at all if it has nothing
        /// to draw). False for anything else, after drawing what was pending: the caller then draws it itself, in order
        bool Submit(IRenderable &renderable);
        /// Draws the pending batch, if any
        void Flush();
        /// Draws what is pending and ends the pass
        void EndPass();

        /// Gives up the GPU meshes and materials, calling the renderer only when `callRenderer` and it still exists
        void Release(bool callRenderer);

        /// Whether sprites are batched at all (true by default). Off draws every sprite alone, as before #445: for comparing, and as
        /// a switch if a game hits a problem with it
        static void SetEnabled(bool enabled);
        [[nodiscard]] static bool IsEnabled();
        /// The most a batch holds, in vertices (a circle is 33, a rect 4)
        static constexpr std::size_t MaxBatchVertices = 65536;

        /// The sprite counts of the latest frame rendered on `renderer`: the frame in progress or, once it has ended, that frame
        /// (Graphics.GetFrameStats in Lua). Zeros for another renderer or before any sprite was drawn
        [[nodiscard]] static SpriteBatchStats GetFrameStats(const Renderer::Common::IRenderer *renderer);

    private:
        struct Slot
        {
            std::shared_ptr<Mesh> mesh;
            std::shared_ptr<Material> material;
            MeshDrawing::DrawResources resources;
        };

        /// Draws `_pending` as one batch in the next slot
        void DrawBatch();
        /// The frame's counters, started again when a new frame begins
        static SpriteBatchStats &CurrentStats();

        Renderer::Common::IRenderer *_renderer = nullptr;
        Renderer::Common::RenderState _state{};
        bool _inPass = false;
        std::vector<Sprite2D *> _pending;
        std::shared_ptr<Material> _pendingMaterial; // the first pending sprite's material: what the others must match
        std::size_t _pendingVertices = 0;
        std::vector<std::unique_ptr<Slot>> _slots;
        std::size_t _slotsUsed = 0;
    };
}
