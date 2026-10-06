#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <renderer/common/ITexture.hpp>
#include <renderer/common/Renderer.hpp>

namespace N2Engine::Text
{
    class Font;
}

namespace N2Engine::Rendering
{
    class Texture;

    /**
     * The GPU resources the engine shares between components: one per (renderer, source asset, kind),
     * refcounted, created on first use and destroyed when the last user releases it. Today it holds textures
     * (Texture assets and fonts' SDF atlases); meshes and materials join it in #3 P2. Engine-internal: the
     * components that draw (UI::Image, TextRenderer, UIText) use it, game code uses them.
     *
     * Lifetime rules, the same for every kind:
     * - An entry keeps its source asset alive (a shared_ptr), so the asset's address (half the key) can't be
     *   reused by another asset while the entry exists.
     * - An entry also keeps its renderer's lifetime token (IRenderer::GetLifetimeToken). A renderer can be
     *   destroyed and a new one made at the same address; an entry whose token has expired is never handed
     *   out, and its resource is never destroyed (the old renderer freed it). The next acquire with that key
     *   replaces it.
     * - A release names the token its handle was acquired with, so a handle from a destroyed renderer never
     *   releases (or destroys) the entry a new renderer at the same address has made since.
     * - Releasing the last user destroys the resource on the renderer only when asked to (callRenderer) and
     *   only while the renderer is alive; a destructor releases without calling the renderer, which then frees
     *   the resource itself when it shuts down.
     *
     * Single-threaded, like the components that use it (main thread). The cache itself is leaked on purpose:
     * a component kept alive by Lua can be destroyed after function-local statics.
     */
    class GpuCache
    {
    public:
        /// What a cache entry holds. P2 adds Mesh and Material.
        enum class ResourceKind : std::uint8_t
        {
            Texture
        };

        /// One user's share of a cache entry: move-only, released when it is destroyed (without calling the
        /// renderer) or by Release. An empty handle (default-constructed, moved from, or a failed acquire)
        /// is false and releases nothing.
        class Handle
        {
        public:
            Handle() = default;
            /// Release(false): the renderer may be gone by now
            ~Handle();
            Handle(Handle &&other) noexcept;
            /// Releases what this handle held (calling the renderer if it is alive), then takes other's share
            Handle &operator=(Handle &&other) noexcept;
            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;

            [[nodiscard]] explicit operator bool() const { return _resource != nullptr; }
            [[nodiscard]] ResourceKind GetKind() const { return _kind; }
            /// The texture, for a Texture handle (nullptr otherwise, or when empty)
            [[nodiscard]] Renderer::Common::ITexture *GetTexture() const;
            /// The source asset the resource was made from (its address; nullptr when empty)
            [[nodiscard]] const void *GetSource() const { return _source; }
            /// The renderer the resource is on (nullptr when empty)
            [[nodiscard]] Renderer::Common::IRenderer *GetRenderer() const { return _renderer; }
            /// Whether this handle holds a resource on `renderer`: the same one, still alive
            [[nodiscard]] bool Holds(const Renderer::Common::IRenderer *renderer) const;

            /// Gives up this share and empties the handle. The last share destroys the resource on the
            /// renderer if callRenderer is true and the renderer is still alive.
            void Release(bool callRenderer);

        private:
            friend class GpuCache;
            Handle(Renderer::Common::IRenderer *renderer, std::weak_ptr<const void> rendererLifetime, ResourceKind kind,
                   const void *source, void *resource);

            Renderer::Common::IRenderer *_renderer = nullptr;
            std::weak_ptr<const void> _rendererLifetime; // the token at acquire time
            ResourceKind _kind = ResourceKind::Texture;
            const void *_source = nullptr;
            void *_resource = nullptr;
        };

        /// A share of `texture`'s GPU texture on `renderer`, created (RGBA8, with the texture's filter, wrap and
        /// mipmaps) on first use. Empty for a null or unloaded texture, or if the renderer can't create it.
        [[nodiscard]] static Handle AcquireTexture(Renderer::Common::IRenderer &renderer,
                                                   const std::shared_ptr<const Texture> &texture);

        /// A share of `font`'s SDF atlas texture on `renderer` (single channel, TextureOptions::SdfAtlas()),
        /// created on first use. Empty for a null or unloaded font, an empty atlas, or if the renderer can't
        /// create it.
        [[nodiscard]] static Handle AcquireFontAtlas(Renderer::Common::IRenderer &renderer,
                                                     const std::shared_ptr<const Text::Font> &font);

        // Introspection, for tests and diagnostics

        /// Every entry, including any left by a destroyed renderer that nothing has replaced or released yet
        [[nodiscard]] static std::size_t GetEntryCount();
        /// The users of `source`'s entry on `renderer` (0 if there is none, or if it belongs to an earlier
        /// renderer at the same address)
        [[nodiscard]] static std::size_t GetUserCount(const Renderer::Common::IRenderer &renderer, const void *source,
                                                      ResourceKind kind = ResourceKind::Texture);

    private:
        /// The shared acquire: finds a live entry and adds a user, or makes the resource with `create` (which
        /// returns nullptr on failure) and a new entry
        static Handle Acquire(Renderer::Common::IRenderer &renderer, ResourceKind kind, std::shared_ptr<const void> source,
                              const std::function<void *(Renderer::Common::IRenderer &)> &create);
        static void Release(Renderer::Common::IRenderer *renderer, const std::weak_ptr<const void> &rendererLifetime,
                            ResourceKind kind, const void *source, bool callRenderer);
    };
}
