#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <math/Vector3.hpp>

#include "renderer/common/RenderTypes.hpp"
#include "renderer/common/RenderState.hpp"
#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/IMesh.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/common/TextureOptions.hpp"

#include "renderer/common/SceneLighting.hpp"

namespace Renderer::Common
{
    struct RenderObject
    {
        IMesh *mesh;
        Transform transform;
        IMaterial *material;
        RenderState state{};
    };

    /**
     * A rendering backend.
     *
     * Draw order: the draws of a frame (DrawMesh and DrawObjects between BeginFrame and EndFrame) reach
     * the framebuffer in submission order, unless a backend documents otherwise. Each draw carries its own
     * RenderState; there are no global depth/cull/blend setters, so a backend that reorders draws still
     * applies the right state to each one.
     *
     * Exceptions:
     * - SoftwareRenderer decides by each draw's state, not its queue. It reorders depth-writing draws
     *   front to back (each run of consecutive draws that both depth-test and write depth; early-Z),
     *   which cannot change the image apart from exact depth ties. Every draw that doesn't both
     *   depth-test and write depth (normally the Transparent queue) keeps its submission position. A
     *   Transparent renderable that doesn't override IRenderable::RenderInQueue draws with the default,
     *   depth-writing state, so it can be reordered. It has no blending: RenderState::blend is ignored.
     *   Its text shader alpha-tests and writes depth wherever a draw depth-tests (see
     *   GetStandardTextShader); text is still ordered by its submitted (Transparent) state.
     * - VulkanRenderer does not draw yet (#43).
     */
    class IRenderer
    {
    public:
        virtual ~IRenderer() = default;

        /**
         * Expires when this renderer is destroyed or shut down (each backend's Shutdown calls EndLifetime, so
         * a renderer initialised again counts as a new one). A component holding resources keeps it next to its
         * IRenderer*: while it hasn't expired the pointer still names this renderer, and once it has, the
         * resources are gone and the pointer must not be called, even if a new renderer now lives at the
         * same address.
         */
        [[nodiscard]] std::weak_ptr<const void> GetLifetimeToken() const { return m_lifetime; }

        // Lifecycle
        virtual bool Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height) = 0;
        virtual void Shutdown() = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
        virtual void Clear(float r, float g, float b, float a) = 0;

        // Frame management
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;
        virtual void Present() = 0;

        // Shader management
        virtual IShader* CreateShaderProgram(const char *vertexSource, const char *fragmentSource) = 0;
        virtual void UseShaderProgram(IShader *shader) = 0;
        virtual bool DestroyShaderProgram(IShader *shader) = 0;
        virtual bool IsValidShader(IShader *shader) const = 0;

        // Resource management
        virtual IMesh* CreateMesh(const MeshData &meshData) = 0;
        virtual void DestroyMesh(IMesh *mesh) = 0;
        virtual ITexture* CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels) = 0;
        virtual void DestroyTexture(ITexture *texture) = 0;
        virtual IMaterial* CreateMaterial(IShader *shader) = 0;
        virtual IMaterial* CreateMaterial(IShader *shader, ITexture *texture) = 0;
        virtual void DestroyMaterial(IMaterial *material) = 0;

        // Rendering
        /// Row-major view and projection for the draws submitted after this call (until the next call), so a
        /// frame can change them between passes (the UI pass draws after the scene with its own projection)
        virtual void SetViewProjection(const float *view, const float *projection) = 0;
        virtual void UpdateSceneLighting(const SceneLightingData &lighting,
                                         const N2Engine::Math::Vector3 &cameraPosition) = 0;

        /// Draws with the default RenderState (RenderState::Opaque(): depth-tested and written, back faces
        /// culled, not blended).
        void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material)
        {
            DrawMesh(mesh, modelMatrix, material, RenderState{});
        }
        /// Draws with the given fixed-function state. The state applies to this draw only.
        virtual void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material,
                              const RenderState &state) = 0;
        /**
         * Draws only the indices in `range` (a submesh: `range.count` indices from `range.first`), with the given
         * state. The OpenGL backend draws them with an offset into the index buffer, and draws nothing for a
         * range that isn't inside the mesh's indices; the software backend loops over them only, clipped to the
         * mesh. Vulkan draws nothing yet (#43).
         *
         * The default body, for backends and test fakes that predate it: a range covering the whole mesh
         * (first 0, count the mesh's index count) draws through the DrawMesh above; any other range draws
         * **nothing**, never the whole mesh, and logs one warning per process.
         */
        virtual void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material, const RenderState &state,
                              const IndexRange &range)
        {
            if (!mesh)
            {
                return;
            }
            if (range.first == 0 && range.count == mesh->GetIndexCount())
            {
                DrawMesh(mesh, modelMatrix, material, state);
                return;
            }
            static std::atomic_flag warned;
            if (!warned.test_and_set())
            {
                std::cerr << GetRendererName()
                          << ": this renderer can't draw part of a mesh (an IndexRange); such draws are skipped"
                          << std::endl;
            }
        }
        /// Draws each object with its own RenderObject::state, in order.
        virtual void DrawObjects(const std::vector<RenderObject> &objects) = 0;
        virtual void OnResize(int width, int height) = 0;

        [[nodiscard]] virtual IShader* GetStandardUnlitShader() const = 0;
        [[nodiscard]] virtual IShader* GetStandardLitShader() const = 0;

        // Text support (#1). These have default bodies so renderers that predate them (and test fakes)
        // keep compiling; the backends override them.

        /**
         * CreateTexture with explicit sampling options (TextureOptions). The overload without options is
         * this with TextureOptions::Default(). The default body ignores the options and calls that
         * overload; a backend that takes options overrides this.
         */
        virtual ITexture* CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels,
                                        const TextureOptions &options)
        {
            static_cast<void>(options);
            return CreateTexture(data, width, height, channels);
        }

        /**
         * Replaces a mesh's vertices and indices in place, keeping the same IMesh*, so a mesh that changes
         * (such as text) needs no destroy and create. Returns false, leaving the mesh unchanged, if `mesh`
         * isn't a live mesh of this renderer, if `meshData` has no vertices (as CreateMesh, which needs
         * some), or if the backend can't update meshes (the default body).
         */
        virtual bool UpdateMesh(IMesh *mesh, const MeshData &meshData)
        {
            static_cast<void>(mesh);
            static_cast<void>(meshData);
            return false;
        }

        /**
         * The built-in SDF text shader: samples a single-channel SDF atlas (the material's texture, read
         * through its first channel) and draws its uAlbedo colour, times the vertex colour, where the
         * distance is above 0.5. Per backend:
         * - OpenGL: antialiased over one screen pixel and blended; writes depth as the draw's state says.
         * - Software: alpha-tested at 0.5 (no blending, no antialiasing), sampled bilinearly; a covered
         *   pixel is written unblended and writes depth whenever the draw depth-tests, even if its state says
         *   no depth write.
         * - Vulkan and the default body: null, as the backend can't draw text yet (#43).
         * Effect uniforms (uOutline, uOutlineColor, uSoftness, uShadowColor, uShadowOffset, uShadowSoftness;
         * see TextDrawing::EffectUniforms in the engine) are all 0 for plain text. OpenGL composites the
         * outline and shadow under the face; the software renderer alpha-tests them and ignores softness.
         */
        [[nodiscard]] virtual IShader* GetStandardTextShader() const { return nullptr; }

        /**
         * Copies the last frame into `buffer` (width * height * 4 bytes). Rows are bottom to top (row 0 is
         * the bottom of the image), as glReadPixels returns them, on every backend. The channel order and
         * scaling differ:
         * - OpenGL: glReadPixels of the framebuffer's bottom-left width x height region, in BGRA order.
         * - Software: the CPU colour buffer, waiting for the frame in flight first, in RGBA order, resampled
         *   (nearest) to width x height.
         * - Vulkan: writes nothing (#43).
         */
        virtual void ReadFramebuffer(std::uint8_t *buffer, int width, int height) const = 0;

        // Debug
        virtual void SetWireframe(bool enabled) = 0;
        [[nodiscard]] virtual const char* GetRendererName() const = 0;

    protected:
        /// Expires every token handed out so far: the renderer's resources are gone (Shutdown). Later calls to
        /// GetLifetimeToken return a fresh token.
        void EndLifetime() { m_lifetime = std::make_shared<const int>(0); }

    private:
        std::shared_ptr<const int> m_lifetime = std::make_shared<const int>(0);
    };
}
