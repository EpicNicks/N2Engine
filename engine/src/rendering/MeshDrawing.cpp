#include "engine/rendering/MeshDrawing.hpp"

#include <utility>

namespace N2Engine::Rendering::MeshDrawing
{
    RenderQueue QueueFor(const Material &material)
    {
        return material.IsBlended() ? RenderQueue::Transparent : RenderQueue::Opaque;
    }

    bool IsMirrored(const Math::Matrix<float, 4, 4> &world)
    {
        const float a = world(0, 0), b = world(0, 1), c = world(0, 2);
        const float d = world(1, 0), e = world(1, 1), f = world(1, 2);
        const float g = world(2, 0), h = world(2, 1), i = world(2, 2);
        const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        return determinant < 0.0f;
    }

    Renderer::Common::RenderState StateFor(const Material &material, const Renderer::Common::RenderState &queueState,
                                           const bool mirrored)
    {
        using Renderer::Common::CullMode;
        Renderer::Common::RenderState state = queueState;
        if (material.IsDoubleSided())
        {
            state.cull = CullMode::None;
        }
        else if (mirrored && state.cull != CullMode::None)
        {
            state.cull = state.cull == CullMode::Back ? CullMode::Front : CullMode::Back;
        }
        return state;
    }

    // ===== DrawResources =====

    DrawResources::~DrawResources()
    {
        Release(false);
    }

    bool DrawResources::Holds(const Renderer::Common::IRenderer *renderer) const
    {
        return renderer && renderer == _renderer && !_rendererLifetime.expired();
    }

    void DrawResources::Bind(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || Holds(renderer))
        {
            return;
        }
        // Another renderer, or a new one at the old one's address: the old shares go (the renderer is only
        // called if it still exists, which Release checks per share)
        Release(true);
        _renderer = renderer;
        _rendererLifetime = renderer->GetLifetimeToken();
    }

    void DrawResources::Release(const bool callRenderer)
    {
        for (GpuCache::Handle &material : _materials)
        {
            material.Release(callRenderer);
        }
        _materials.clear();
        _mesh.Release(callRenderer);
        _renderer = nullptr;
        _rendererLifetime.reset();
        _failedMesh.reset();
        _failedMeshVersion = 0;
    }

    Renderer::Common::IMaterial *DrawResources::EnsureMaterial(const std::size_t index,
                                                               const std::shared_ptr<const Material> &material)
    {
        GpuCache::Handle &handle = _materials[index];
        if (!handle.Holds(_renderer) || handle.GetSource() != material.get() ||
            handle.GetVersion() != material->GetVersion())
        {
            // A new slot, another material, or a changed one: the old share goes first, so the last user of an
            // old version destroys it
            handle.Release(true);
            handle = GpuCache::AcquireMaterial(*_renderer, material);
        }
        return handle.GetMaterial();
    }

    std::size_t DrawResources::Draw(const std::shared_ptr<const Mesh> &mesh,
                                    const std::span<const std::shared_ptr<Material>> materials,
                                    const std::shared_ptr<const Material> &fallback,
                                    const Math::Matrix<float, 4, 4> &world, const Common::Color &tint,
                                    const RenderQueue queue, const Renderer::Common::RenderState &queueState)
    {
        if (!_renderer || _rendererLifetime.expired() || !mesh || mesh->GetSubmeshCount() == 0)
        {
            return 0;
        }

        if (!_mesh.Holds(_renderer) || _mesh.GetSource() != mesh.get())
        {
            const bool failedBefore = !_failedMesh.expired() && _failedMesh.lock() == mesh &&
                                      _failedMeshVersion == mesh->GetVersion();
            if (failedBefore)
            {
                return 0;
            }
            _mesh.Release(true);
            _mesh = GpuCache::AcquireMesh(*_renderer, mesh);
            if (!_mesh)
            {
                _failedMesh = mesh;
                _failedMeshVersion = mesh->GetVersion();
                return 0;
            }
            _failedMesh.reset();
        }
        GpuCache::SyncMesh(_mesh); // re-uploads a changed mesh; the handle then names the current GPU mesh
        Renderer::Common::IMesh *gpuMesh = _mesh.GetMesh();
        if (!gpuMesh)
        {
            return 0;
        }

        const std::span<const Submesh> submeshes = mesh->GetSubmeshes();
        if (_materials.size() > submeshes.size())
        {
            // Fewer submeshes than before (the mesh changed): the extra shares go, calling the renderer
            for (std::size_t i = submeshes.size(); i < _materials.size(); ++i)
            {
                _materials[i].Release(true);
            }
        }
        _materials.resize(submeshes.size());

        const bool mirrored = IsMirrored(world);
        std::size_t drawn = 0;
        for (std::size_t i = 0; i < submeshes.size(); ++i)
        {
            const std::shared_ptr<const Material> material =
                i < materials.size() && materials[i] ? std::shared_ptr<const Material>(materials[i]) : fallback;
            if (!material || QueueFor(*material) != queue)
            {
                continue;
            }
            Renderer::Common::IMaterial *gpuMaterial = EnsureMaterial(i, material);
            if (!gpuMaterial)
            {
                continue;
            }
            material->ApplyUniforms(*gpuMaterial, tint);
            const Submesh &submesh = submeshes[i];
            _renderer->DrawMesh(gpuMesh, world.Data(), gpuMaterial, StateFor(*material, queueState, mirrored),
                                Renderer::Common::IndexRange{submesh.firstIndex, submesh.indexCount});
            ++drawn;
        }
        return drawn;
    }
}
