#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/Renderer.hpp>

namespace N2Engine::Text
{
    class Font;
}

namespace N2Engine::Rendering
{
    class Material;
    class Mesh;
    class Texture;

    /**
     * The GPU resources the engine shares between components: one per (renderer, source asset, kind),
     * refcounted, created on first use and destroyed when the last user releases it. It holds textures (Texture
     * assets and fonts' SDF atlases), meshes (one IMesh per Mesh, re-uploaded when the mesh's version changes)
     * and materials (one IMaterial per Material and version, holding a share of its base colour texture).
     * Engine-internal: the components that draw (MeshRenderer, the built-in shapes, UI::Image, TextRenderer,
     * UIText) use it, game code uses them.
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
        /// What a cache entry holds
        enum class ResourceKind : std::uint8_t
        {
            Texture,
            Mesh,
            Material
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

            explicit operator bool() const { return _resource != nullptr; }
            [[nodiscard]] ResourceKind GetKind() const { return _kind; }
            /// The texture, for a Texture handle (nullptr otherwise, or when empty)
            [[nodiscard]] Renderer::Common::ITexture *GetTexture() const;
            /// The mesh, for a Mesh handle (nullptr otherwise, or when empty), as of this handle's last SyncMesh (or
            /// acquire). It can be stale: when another handle's sync replaced the entry's IMesh (a backend that can't
            /// update meshes), this one still names the old, destroyed IMesh until its own SyncMesh. Call SyncMesh
            /// before drawing with it.
            [[nodiscard]] Renderer::Common::IMesh *GetMesh() const;
            /// The material, for a Material handle (nullptr otherwise, or when empty)
            [[nodiscard]] Renderer::Common::IMaterial *GetMaterial() const;
            /// The source's version the entry was made for: a Material's GPU version (Material::GetGpuVersion) for a
            /// Material handle, 0 for the other kinds (a mesh entry follows its mesh's versions, see SyncMesh)
            [[nodiscard]] std::uint64_t GetVersion() const { return _version; }
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
                   const void *source, std::uint64_t version, void *resource);

            Renderer::Common::IRenderer *_renderer = nullptr;
            std::weak_ptr<const void> _rendererLifetime; // the token at acquire time
            ResourceKind _kind = ResourceKind::Texture;
            const void *_source = nullptr;
            std::uint64_t _version = 0; // part of the key (a Material's GPU version; 0 for the other kinds)
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

        /**
         * A share of `mesh`'s GPU mesh on `renderer` (IRenderer::CreateMesh of its vertices and indices), created
         * on first use and shared by every user of the mesh there. Brought up to the mesh's current version
         * (SyncMesh) on every acquire. Empty for a null mesh, one without geometry, or if the renderer can't
         * create it.
         */
        [[nodiscard]] static Handle AcquireMesh(Renderer::Common::IRenderer &renderer,
                                                const std::shared_ptr<const Mesh> &mesh);

        /**
         * Re-uploads a Mesh handle's mesh if it has changed (its version is newer than the GPU copy's): in place
         * with IRenderer::UpdateMesh, or, where the backend can't, as a new IMesh replacing the old one (which is
         * destroyed). Then points the handle at the current IMesh. If both fail, the old copy stays and the version
         * isn't recorded, so the next sync tries again. Every user calls it before drawing, so one
         * user's sync also serves the others; a handle whose IMesh another user's sync replaced is repointed by
         * its own next sync. False for an empty, non-Mesh or dead handle.
         */
        static bool SyncMesh(Handle &handle);

        /**
         * A share of the GPU material for `material`'s current GPU version (Material::GetGpuVersion: its shading
         * and its textures) on `renderer`: the standard lit or unlit shader (by the material's shading) with
         * its base colour texture and, for lit, its emissive and occlusion textures (each a share of the texture's
         * own cache entry, held by the material's entry), and its
         * uniforms set (Material::ApplyUniforms). Each GPU version is a separate entry, so a material whose shader
         * or texture changed gets a new GPU material when its users acquire again, and the old one goes with its
         * last user. Its other fields are uniforms and render state, set per draw, so changing them makes nothing
         * new. Empty for a null material, or if the renderer has no such shader or can't create the material. A
         * base colour texture the renderer can't create is left out.
         */
        [[nodiscard]] static Handle AcquireMaterial(Renderer::Common::IRenderer &renderer,
                                                    const std::shared_ptr<const Material> &material);

        // Introspection, for tests and diagnostics

        /// Every entry, including any left by a destroyed renderer that nothing has replaced or released yet
        [[nodiscard]] static std::size_t GetEntryCount();
        /// The users of `source`'s entry on `renderer` (0 if there is none, or if it belongs to an earlier
        /// renderer at the same address). `version` is a Material's GPU version for a Material entry, else 0.
        [[nodiscard]] static std::size_t GetUserCount(const Renderer::Common::IRenderer &renderer, const void *source,
                                                      ResourceKind kind = ResourceKind::Texture,
                                                      std::uint64_t version = 0);

    private:
        /// What makes an entry's resource: returns it (nullptr on failure), and may give the entry a share of
        /// another entry to hold (a material's texture)
        using CreateFunction = std::function<void *(Renderer::Common::IRenderer &, std::vector<Handle> &dependencies)>;

        /// The shared acquire: finds a live entry and adds a user, or makes the resource with `create` and a new
        /// entry. `uploadedVersion` is recorded on a new entry (a mesh's version).
        static Handle Acquire(Renderer::Common::IRenderer &renderer, ResourceKind kind, std::uint64_t version,
                              std::shared_ptr<const void> source, const CreateFunction &create,
                              std::uint64_t uploadedVersion = 0);
        static void Release(Renderer::Common::IRenderer *renderer, const std::weak_ptr<const void> &rendererLifetime,
                            ResourceKind kind, const void *source, std::uint64_t version, bool callRenderer);
    };
}
