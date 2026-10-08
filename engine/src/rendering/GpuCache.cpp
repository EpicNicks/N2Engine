#include "engine/rendering/GpuCache.hpp"

#include <map>
#include <tuple>
#include <utility>

#include <renderer/common/TextureOptions.hpp>

#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/text/Font.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        using Renderer::Common::IMaterial;
        using Renderer::Common::IMesh;
        using Renderer::Common::IRenderer;
        using Renderer::Common::IShader;
        using Renderer::Common::ITexture;

        struct Entry
        {
            std::shared_ptr<const void> source;          // keeps the source's address (half the key) valid
            std::weak_ptr<const void> rendererLifetime;  // expired once the renderer that made it is gone
            void *resource = nullptr;
            std::size_t users = 0;
            std::uint64_t uploadedVersion = 0; // Mesh: the mesh version the GPU copy holds
            std::vector<GpuCache::Handle> dependencies; // Material: its shares of its textures' entries
        };

        // (renderer, source, kind, version): the version is a Material's, 0 for the other kinds
        using Key = std::tuple<const IRenderer *, const void *, GpuCache::ResourceKind, std::uint64_t>;

        std::map<Key, Entry> &Entries()
        {
            // Leaked on purpose: a component kept alive by Lua can be destroyed after function-local statics
            static auto *entries = new std::map<Key, Entry>();
            return *entries;
        }

        bool SameOwner(const std::weak_ptr<const void> &a, const std::weak_ptr<const void> &b)
        {
            return !a.owner_before(b) && !b.owner_before(a);
        }

        void Destroy(IRenderer &renderer, const GpuCache::ResourceKind kind, void *resource)
        {
            switch (kind)
            {
            case GpuCache::ResourceKind::Texture:
                renderer.DestroyTexture(static_cast<ITexture *>(resource));
                break;
            case GpuCache::ResourceKind::Mesh:
                renderer.DestroyMesh(static_cast<IMesh *>(resource));
                break;
            case GpuCache::ResourceKind::Material:
                renderer.DestroyMaterial(static_cast<IMaterial *>(resource));
                break;
            }
        }
    }

    // ===== Handle =====

    GpuCache::Handle::Handle(IRenderer *renderer, std::weak_ptr<const void> rendererLifetime, const ResourceKind kind,
                             const void *source, const std::uint64_t version, void *resource)
        : _renderer(renderer), _rendererLifetime(std::move(rendererLifetime)), _kind(kind), _source(source),
          _version(version), _resource(resource)
    {
    }

    GpuCache::Handle::~Handle()
    {
        Release(false);
    }

    GpuCache::Handle::Handle(Handle &&other) noexcept
        : _renderer(std::exchange(other._renderer, nullptr)), _rendererLifetime(std::move(other._rendererLifetime)),
          _kind(other._kind), _source(std::exchange(other._source, nullptr)),
          _version(std::exchange(other._version, 0)), _resource(std::exchange(other._resource, nullptr))
    {
        other._rendererLifetime.reset();
    }

    GpuCache::Handle &GpuCache::Handle::operator=(Handle &&other) noexcept
    {
        if (this != &other)
        {
            Release(true);
            _renderer = std::exchange(other._renderer, nullptr);
            _rendererLifetime = std::move(other._rendererLifetime);
            other._rendererLifetime.reset();
            _kind = other._kind;
            _source = std::exchange(other._source, nullptr);
            _version = std::exchange(other._version, 0);
            _resource = std::exchange(other._resource, nullptr);
        }
        return *this;
    }

    ITexture *GpuCache::Handle::GetTexture() const
    {
        return _kind == ResourceKind::Texture ? static_cast<ITexture *>(_resource) : nullptr;
    }

    IMesh *GpuCache::Handle::GetMesh() const
    {
        return _kind == ResourceKind::Mesh ? static_cast<IMesh *>(_resource) : nullptr;
    }

    IMaterial *GpuCache::Handle::GetMaterial() const
    {
        return _kind == ResourceKind::Material ? static_cast<IMaterial *>(_resource) : nullptr;
    }

    bool GpuCache::Handle::Holds(const IRenderer *renderer) const
    {
        return _resource && renderer && renderer == _renderer && !_rendererLifetime.expired();
    }

    void GpuCache::Handle::Release(const bool callRenderer)
    {
        if (_source)
        {
            GpuCache::Release(_renderer, _rendererLifetime, _kind, _source, _version, callRenderer);
        }
        _renderer = nullptr;
        _rendererLifetime.reset();
        _source = nullptr;
        _version = 0;
        _resource = nullptr;
    }

    // ===== GpuCache =====

    GpuCache::Handle GpuCache::Acquire(IRenderer &renderer, const ResourceKind kind, const std::uint64_t version,
                                       std::shared_ptr<const void> source, const CreateFunction &create,
                                       const std::uint64_t uploadedVersion)
    {
        auto &entries = Entries();
        const Key key{&renderer, source.get(), kind, version};
        std::weak_ptr<const void> lifetime = renderer.GetLifetimeToken();
        if (const auto it = entries.find(key); it != entries.end())
        {
            if (SameOwner(it->second.rendererLifetime, lifetime))
            {
                ++it->second.users;
                return Handle(&renderer, std::move(lifetime), kind, source.get(), version, it->second.resource);
            }
            // Left by a destroyed renderer at the same address: its resource went with it. What the entry held of
            // another entry is let go after the erase (releasing it changes the map).
            std::vector<Handle> staleDependencies = std::move(it->second.dependencies);
            entries.erase(it);
            for (Handle &stale : staleDependencies)
            {
                stale.Release(false);
            }
        }

        std::vector<Handle> dependencies;
        void *resource = create(renderer, dependencies);
        if (!resource)
        {
            for (Handle &dependency : dependencies)
            {
                dependency.Release(true);
            }
            return {};
        }
        const void *sourceAddress = source.get();
        Entry entry;
        entry.source = std::move(source);
        entry.rendererLifetime = lifetime;
        entry.resource = resource;
        entry.users = 1;
        entry.uploadedVersion = uploadedVersion;
        entry.dependencies = std::move(dependencies);
        entries.emplace(key, std::move(entry));
        return Handle(&renderer, std::move(lifetime), kind, sourceAddress, version, resource);
    }

    void GpuCache::Release(IRenderer *renderer, const std::weak_ptr<const void> &rendererLifetime, const ResourceKind kind,
                           const void *source, const std::uint64_t version, const bool callRenderer)
    {
        auto &entries = Entries();
        const auto it = entries.find(Key{renderer, source, kind, version});
        // Not the caller's entry if a new renderer at the same address has made one since
        if (it == entries.end() || !SameOwner(it->second.rendererLifetime, rendererLifetime))
        {
            return;
        }
        if (--it->second.users > 0)
        {
            return;
        }
        if (callRenderer && renderer && !rendererLifetime.expired())
        {
            Destroy(*renderer, kind, it->second.resource);
        }
        // The entry's share of another entry (a material's texture) goes after the erase: releasing it changes
        // the map, and the texture must outlive the material that samples it
        std::vector<Handle> dependencies = std::move(it->second.dependencies);
        entries.erase(it);
        for (Handle &dependency : dependencies)
        {
            dependency.Release(callRenderer);
        }
    }

    GpuCache::Handle GpuCache::AcquireTexture(IRenderer &renderer, const std::shared_ptr<const Texture> &texture)
    {
        if (!texture || !texture->IsLoaded())
        {
            return {};
        }
        return Acquire(renderer, ResourceKind::Texture, 0, texture, [&texture](IRenderer &target, std::vector<Handle> &) -> void *
        {
            return target.CreateTexture(texture->GetPixels().data(), texture->GetWidth(), texture->GetHeight(),
                                        Texture::GetChannels(), texture->GetTextureOptions());
        });
    }

    GpuCache::Handle GpuCache::AcquireFontAtlas(IRenderer &renderer, const std::shared_ptr<const Text::Font> &font)
    {
        if (!font || !font->IsLoaded())
        {
            return {};
        }
        return Acquire(renderer, ResourceKind::Texture, 0, font, [&font](IRenderer &target, Handle &) -> void *
        {
            const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
            if (atlas.GetWidth() <= 0 || atlas.GetHeight() <= 0 || atlas.GetPixels().empty())
            {
                return nullptr;
            }
            return target.CreateTexture(atlas.GetPixels().data(), static_cast<uint32_t>(atlas.GetWidth()),
                                        static_cast<uint32_t>(atlas.GetHeight()), 1,
                                        Renderer::Common::TextureOptions::SdfAtlas());
        });
    }

    GpuCache::Handle GpuCache::AcquireMesh(IRenderer &renderer, const std::shared_ptr<const Mesh> &mesh)
    {
        if (!mesh || mesh->GetVertices().empty() || mesh->GetIndices().empty())
        {
            return {};
        }
        Handle handle = Acquire(renderer, ResourceKind::Mesh, 0, mesh, [&mesh](IRenderer &target, Handle &) -> void *
        {
            return target.CreateMesh(mesh->GetMeshData());
        }, mesh->GetVersion());
        SyncMesh(handle);
        return handle;
    }

    bool GpuCache::SyncMesh(Handle &handle)
    {
        if (handle._kind != ResourceKind::Mesh || !handle._source || !handle._renderer ||
            handle._rendererLifetime.expired())
        {
            return false;
        }
        auto &entries = Entries();
        const auto it = entries.find(Key{handle._renderer, handle._source, ResourceKind::Mesh, 0});
        if (it == entries.end() || !SameOwner(it->second.rendererLifetime, handle._rendererLifetime))
        {
            return false;
        }
        Entry &entry = it->second;
        const auto *mesh = static_cast<const Mesh *>(entry.source.get());
        if (entry.uploadedVersion != mesh->GetVersion())
        {
            IRenderer &renderer = *handle._renderer;
            auto *current = static_cast<IMesh *>(entry.resource);
            bool uploaded = renderer.UpdateMesh(current, mesh->GetMeshData());
            if (!uploaded)
            {
                // A backend that can't update meshes gets a new one; if even that fails, the old one stays
                if (IMesh *replacement = renderer.CreateMesh(mesh->GetMeshData()))
                {
                    renderer.DestroyMesh(current);
                    entry.resource = replacement;
                    uploaded = true;
                }
            }
            // Only an upload that happened records the version: a failed one is tried again on the next sync, and
            // the GPU copy is never taken for the current geometry when it isn't
            if (uploaded)
            {
                entry.uploadedVersion = mesh->GetVersion();
            }
        }
        handle._resource = entry.resource;
        return handle._resource != nullptr;
    }

    GpuCache::Handle GpuCache::AcquireMaterial(IRenderer &renderer, const std::shared_ptr<const Material> &material)
    {
        if (!material)
        {
            return {};
        }
        return Acquire(renderer, ResourceKind::Material, material->GetGpuVersion(), material,
                       [&material](IRenderer &target, std::vector<Handle> &dependencies) -> void *
        {
            IShader *shader = material->GetShading() == ShadingModel::Lit ? target.GetStandardLitShader()
                                                                           : target.GetStandardUnlitShader();
            if (!shader)
            {
                return nullptr;
            }
            // A texture the renderer can't create is left out. Each texture is a share of its own entry, held by
            // this material's entry (and so alive as long as the material that samples it).
            const auto acquire = [&](const std::shared_ptr<Texture> &source) -> ITexture *
            {
                if (!source)
                {
                    return nullptr;
                }
                Handle handle = AcquireTexture(target, source);
                ITexture *texture = handle.GetTexture();
                if (texture)
                {
                    dependencies.push_back(std::move(handle));
                }
                return texture;
            };
            ITexture *texture = acquire(material->GetBaseColorTexture());
            IMaterial *created = target.CreateMaterial(shader, texture);
            if (created)
            {
                // Only the lit shader reads them
                if (material->GetShading() == ShadingModel::Lit)
                {
                    created->SetAuxTexture(Renderer::Common::AuxTexture::Emissive, acquire(material->GetEmissiveTexture()));
                    created->SetAuxTexture(Renderer::Common::AuxTexture::Occlusion, acquire(material->GetOcclusionTexture()));
                }
                material->ApplyUniforms(*created);
            }
            return created;
        });
    }

    std::size_t GpuCache::GetEntryCount()
    {
        return Entries().size();
    }

    std::size_t GpuCache::GetUserCount(const IRenderer &renderer, const void *source, const ResourceKind kind,
                                       const std::uint64_t version)
    {
        const auto &entries = Entries();
        const auto it = entries.find(Key{&renderer, source, kind, version});
        if (it == entries.end() || !SameOwner(it->second.rendererLifetime, renderer.GetLifetimeToken()))
        {
            return 0;
        }
        return it->second.users;
    }
}
