#include "engine/rendering/GpuCache.hpp"

#include <map>
#include <tuple>
#include <utility>

#include <renderer/common/TextureOptions.hpp>

#include "engine/rendering/Texture.hpp"
#include "engine/text/Font.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        using Renderer::Common::IRenderer;
        using Renderer::Common::ITexture;

        struct Entry
        {
            std::shared_ptr<const void> source;          // keeps the source's address (half the key) valid
            std::weak_ptr<const void> rendererLifetime;  // expired once the renderer that made it is gone
            void *resource = nullptr;
            std::size_t users = 0;
        };

        using Key = std::tuple<const IRenderer *, const void *, GpuCache::ResourceKind>;

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
            }
        }
    }

    // ===== Handle =====

    GpuCache::Handle::Handle(IRenderer *renderer, std::weak_ptr<const void> rendererLifetime, const ResourceKind kind,
                             const void *source, void *resource)
        : _renderer(renderer), _rendererLifetime(std::move(rendererLifetime)), _kind(kind), _source(source),
          _resource(resource)
    {
    }

    GpuCache::Handle::~Handle()
    {
        Release(false);
    }

    GpuCache::Handle::Handle(Handle &&other) noexcept
        : _renderer(std::exchange(other._renderer, nullptr)), _rendererLifetime(std::move(other._rendererLifetime)),
          _kind(other._kind), _source(std::exchange(other._source, nullptr)),
          _resource(std::exchange(other._resource, nullptr))
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
            _resource = std::exchange(other._resource, nullptr);
        }
        return *this;
    }

    ITexture *GpuCache::Handle::GetTexture() const
    {
        return _kind == ResourceKind::Texture ? static_cast<ITexture *>(_resource) : nullptr;
    }

    bool GpuCache::Handle::Holds(const IRenderer *renderer) const
    {
        return _resource && renderer && renderer == _renderer && !_rendererLifetime.expired();
    }

    void GpuCache::Handle::Release(const bool callRenderer)
    {
        if (_source)
        {
            GpuCache::Release(_renderer, _rendererLifetime, _kind, _source, callRenderer);
        }
        _renderer = nullptr;
        _rendererLifetime.reset();
        _source = nullptr;
        _resource = nullptr;
    }

    // ===== GpuCache =====

    GpuCache::Handle GpuCache::Acquire(IRenderer &renderer, const ResourceKind kind, std::shared_ptr<const void> source,
                                       const std::function<void *(IRenderer &)> &create)
    {
        auto &entries = Entries();
        const Key key{&renderer, source.get(), kind};
        std::weak_ptr<const void> lifetime = renderer.GetLifetimeToken();
        if (const auto it = entries.find(key); it != entries.end())
        {
            if (SameOwner(it->second.rendererLifetime, lifetime))
            {
                ++it->second.users;
                return Handle(&renderer, std::move(lifetime), kind, source.get(), it->second.resource);
            }
            // Left by a destroyed renderer at the same address: its resource went with it
            entries.erase(it);
        }

        void *resource = create(renderer);
        if (!resource)
        {
            return {};
        }
        const void *sourceAddress = source.get();
        entries.emplace(key, Entry{std::move(source), lifetime, resource, 1});
        return Handle(&renderer, std::move(lifetime), kind, sourceAddress, resource);
    }

    void GpuCache::Release(IRenderer *renderer, const std::weak_ptr<const void> &rendererLifetime, const ResourceKind kind,
                           const void *source, const bool callRenderer)
    {
        auto &entries = Entries();
        const auto it = entries.find(Key{renderer, source, kind});
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
        entries.erase(it);
    }

    GpuCache::Handle GpuCache::AcquireTexture(IRenderer &renderer, const std::shared_ptr<const Texture> &texture)
    {
        if (!texture || !texture->IsLoaded())
        {
            return {};
        }
        return Acquire(renderer, ResourceKind::Texture, texture, [&texture](IRenderer &target) -> void *
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
        return Acquire(renderer, ResourceKind::Texture, font, [&font](IRenderer &target) -> void *
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

    std::size_t GpuCache::GetEntryCount()
    {
        return Entries().size();
    }

    std::size_t GpuCache::GetUserCount(const IRenderer &renderer, const void *source, const ResourceKind kind)
    {
        const auto &entries = Entries();
        const auto it = entries.find(Key{&renderer, source, kind});
        if (it == entries.end() || !SameOwner(it->second.rendererLifetime, renderer.GetLifetimeToken()))
        {
            return 0;
        }
        return it->second.users;
    }
}
