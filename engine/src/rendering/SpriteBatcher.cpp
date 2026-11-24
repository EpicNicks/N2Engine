#include "engine/rendering/SpriteBatcher.hpp"

#include <optional>
#include <span>
#include <utility>

#include "engine/IRenderable.hpp"
#include "engine/rendering/Sprite2D.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        bool s_enabled = true;

        /// A frame's sprite counts and which frame of which renderer they are of: the renderer's finished-frame count when the frame
        /// began (IRenderer::GetLastFrameCounters), so a new frame is seen by that count moving on
        struct TaggedStats
        {
            const Renderer::Common::IRenderer *renderer = nullptr;
            /// Which renderer, not just which address: a renderer made where an earlier one stood has the same address and, being new,
            /// the same frame count, and must not be mistaken for it (its token is another one, and the weak pointer keeps the old
            /// token's control block, so its address is never reused while the stats are held)
            std::weak_ptr<const void> life;
            std::uint64_t tag = 0;
            SpriteBatchStats stats{};

            [[nodiscard]] bool Is(const Renderer::Common::IRenderer *other) const
            {
                if (renderer != other || other == nullptr)
                {
                    return false;
                }
                const std::weak_ptr<const void> now = other->GetLifetimeToken();
                return !life.owner_before(now) && !now.owner_before(life);
            }
        };
        TaggedStats s_current;
        TaggedStats s_previous;
        // A renderer that does not count its frames gets a new tag every pass
        std::uint64_t s_untaggedPasses = 0;

        std::uint64_t FrameTag(const Renderer::Common::IRenderer &renderer)
        {
            return renderer.SupportsRenderCounters() ? renderer.GetLastFrameCounters().frames : ~(++s_untaggedPasses);
        }

        bool SameFlash(const Common::Color &a, const Common::Color &b)
        {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        }

        /// Whether a sprite drawing with `b` may join a batch drawing with `a`: everything a draw call fixes for every pixel is equal
        bool SameLook(const Material &a, const Material &b)
        {
            return a.GetBaseColorTexture() == b.GetBaseColorTexture() && a.GetBlendMode() == b.GetBlendMode() &&
                   a.GetSaturation() == b.GetSaturation() && SameFlash(a.GetFlash(), b.GetFlash()) &&
                   a.GetBlendDiscardBelow() == b.GetBlendDiscardBelow();
        }

        /// Gives `to` (a batch slot's material) what the batch draws with, each only when it changed
        void SyncLook(Material &to, const Material &from)
        {
            if (to.GetBaseColorTexture() != from.GetBaseColorTexture())
            {
                to.SetBaseColorTexture(from.GetBaseColorTexture());
            }
            if (to.GetBlendMode() != from.GetBlendMode())
            {
                to.SetBlendMode(from.GetBlendMode());
            }
            if (to.GetSaturation() != from.GetSaturation())
            {
                to.SetSaturation(from.GetSaturation());
            }
            if (!SameFlash(to.GetFlash(), from.GetFlash()))
            {
                to.SetFlash(from.GetFlash());
            }
            if (to.GetBlendDiscardBelow() != from.GetBlendDiscardBelow())
            {
                to.SetBlendDiscardBelow(from.GetBlendDiscardBelow());
            }
        }
    }

    SpriteBatcher::~SpriteBatcher()
    {
        Release(true);
    }

    void SpriteBatcher::SetEnabled(const bool enabled)
    {
        s_enabled = enabled;
    }

    bool SpriteBatcher::IsEnabled()
    {
        return s_enabled;
    }

    SpriteBatchStats &SpriteBatcher::CurrentStats()
    {
        return s_current.stats;
    }

    SpriteBatchStats SpriteBatcher::GetFrameStats(const Renderer::Common::IRenderer *renderer)
    {
        if (renderer == nullptr)
        {
            return {};
        }
        if (s_current.Is(renderer))
        {
            return s_current.stats;
        }
        if (s_previous.Is(renderer))
        {
            return s_previous.stats;
        }
        return {};
    }

    void SpriteBatcher::BeginPass(Renderer::Common::IRenderer &renderer, const Renderer::Common::RenderState &state)
    {
        const std::uint64_t tag = FrameTag(renderer);
        const bool newFrame = !s_current.Is(&renderer) || s_current.tag != tag;
        if (newFrame)
        {
            s_previous = s_current;
            s_current = TaggedStats{&renderer, renderer.GetLifetimeToken(), tag, {}};
        }
        // A batch's mesh may be read only when the frame ends (the software renderer rasterizes then), so within one frame every batch
        // takes a slot of its own, across passes too (two cameras); a new frame, or another renderer, starts from the first slot
        if (_renderer != &renderer || newFrame || !renderer.SupportsRenderCounters())
        {
            _slotsUsed = 0;
        }
        _renderer = &renderer;
        _state = state;
        _inPass = true;
        _pending.clear();
        _pendingMaterial.reset();
        _pendingVertices = 0;
    }

    bool SpriteBatcher::Submit(IRenderable &renderable)
    {
        auto *sprite = dynamic_cast<Sprite2D *>(&renderable);
        if (!_inPass || !_renderer || !sprite || !s_enabled || sprite->GetShader() != nullptr)
        {
            Flush();
            if (sprite && _inPass && _renderer && sprite->GetModelMatrix() && sprite->GetMesh())
            {
                ++CurrentStats().sprites;
                ++CurrentStats().drawCalls;
            }
            return false;
        }
        const std::optional<Positionable::Matrix4> world = sprite->GetModelMatrix();
        const std::shared_ptr<Mesh> mesh = world ? sprite->GetMesh() : nullptr;
        if (!mesh)
        {
            return true; // nothing to draw, as RenderInQueue would draw nothing; the batch goes on around it
        }
        const std::shared_ptr<Material> &material = sprite->EnsureMaterial(*_renderer);
        const std::size_t vertices = mesh->GetVertices().size();
        if (_pendingMaterial && (!SameLook(*_pendingMaterial, *material) || _pendingVertices + vertices > MaxBatchVertices))
        {
            Flush();
        }
        if (!_pendingMaterial)
        {
            _pendingMaterial = material;
        }
        _pending.push_back(sprite);
        _pendingVertices += vertices;
        return true;
    }

    void SpriteBatcher::Flush()
    {
        if (_pending.empty() || !_renderer)
        {
            _pending.clear();
            _pendingMaterial.reset();
            _pendingVertices = 0;
            return;
        }
        SpriteBatchStats &stats = CurrentStats();
        stats.sprites += static_cast<std::uint32_t>(_pending.size());
        ++stats.drawCalls;
        if (_pending.size() == 1)
        {
            // Alone: the sprite draws itself, exactly as without batching
            _pending.front()->RenderInQueue(_renderer, _state, RenderQueue::Transparent);
        }
        else
        {
            ++stats.batches;
            stats.batchedSprites += static_cast<std::uint32_t>(_pending.size());
            DrawBatch();
        }
        _pending.clear();
        _pendingMaterial.reset();
        _pendingVertices = 0;
    }

    void SpriteBatcher::DrawBatch()
    {
        Renderer::Common::MeshData data;
        data.vertices.reserve(_pendingVertices);
        for (Sprite2D *sprite : _pending)
        {
            const std::optional<Positionable::Matrix4> world = sprite->GetModelMatrix();
            const std::shared_ptr<Mesh> mesh = sprite->GetMesh();
            if (!world || !mesh)
            {
                continue;
            }
            const Positionable::Matrix4 &m = *world;
            const Common::Color &color = sprite->GetColor();
            const auto base = static_cast<std::uint32_t>(data.vertices.size());
            for (Renderer::Common::Vertex vertex : mesh->GetVertices())
            {
                const float x = vertex.position[0], y = vertex.position[1], z = vertex.position[2];
                // The world matrix applied on the CPU (row-major, column vectors: what the backends do with the model matrix)
                vertex.position[0] = m(0, 0) * x + m(0, 1) * y + m(0, 2) * z + m(0, 3);
                vertex.position[1] = m(1, 0) * x + m(1, 1) * y + m(1, 2) * z + m(1, 3);
                vertex.position[2] = m(2, 0) * x + m(2, 1) * y + m(2, 2) * z + m(2, 3);
                // The sprite's colour, which the unlit shaders multiply in as uAlbedo, rides in the vertex colour instead: the same
                // product, uAlbedo x texture x vertex colour, with a white uAlbedo
                vertex.color[0] *= color.r;
                vertex.color[1] *= color.g;
                vertex.color[2] *= color.b;
                vertex.color[3] *= color.a;
                data.vertices.push_back(vertex);
            }
            for (const std::uint32_t index : mesh->GetIndices())
            {
                data.indices.push_back(base + index);
            }
        }
        if (data.indices.empty())
        {
            return;
        }

        if (_slotsUsed == _slots.size())
        {
            _slots.push_back(std::make_unique<Slot>());
        }
        Slot &slot = *_slots[_slotsUsed++];
        if (!slot.mesh)
        {
            slot.mesh = Mesh::Create(std::move(data));
        }
        else if (!slot.mesh->SetData(std::move(data)))
        {
            return;
        }
        if (!slot.mesh)
        {
            return;
        }
        if (!slot.material)
        {
            slot.material = Material::Create(ShadingModel::Unlit);
            slot.material->SetAlphaMode(AlphaMode::Blend);
            slot.material->SetDoubleSided(true);
        }
        SyncLook(*slot.material, *_pendingMaterial);

        Renderer::Common::RenderState state = _state;
        state.softwareBlend = true; // as Sprite2D::RenderInQueue: sprites blend on the software renderer too
        slot.resources.Bind(_renderer);
        slot.resources.Draw(slot.mesh, std::span<const std::shared_ptr<Material>>(&slot.material, 1), Material::GetDefaultUnlit(),
                            Positionable::Matrix4::identity(), Common::Color::White, RenderQueue::Transparent, state);
    }

    void SpriteBatcher::EndPass()
    {
        Flush();
        _inPass = false;
    }

    void SpriteBatcher::Release(const bool callRenderer)
    {
        for (const std::unique_ptr<Slot> &slot : _slots)
        {
            slot->resources.Release(callRenderer);
        }
        _slots.clear();
        _slotsUsed = 0;
        _pending.clear();
        _pendingMaterial.reset();
        _renderer = nullptr;
    }
}
