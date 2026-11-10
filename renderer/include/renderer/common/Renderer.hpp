#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <math/Vector3.hpp>

#include "renderer/common/GpuCompute.hpp"
#include "renderer/common/RenderTypes.hpp"
#include "renderer/common/RenderCounters.hpp"
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
     * - VulkanRenderer renders into its own offscreen target and draws in submission order, like OpenGL (#43). It
     *   draws the unlit, lit and text shaders' materials with OpenGL's blending (stages B-E); text is antialiased and blended
     *   like OpenGL's (see GetStandardTextShader). With a window, Present blits the offscreen frame to a swapchain image
     *   (stage F).
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
         * state. The OpenGL and Vulkan backends draw them with an offset into the index buffer, and draw nothing for
         * a range that isn't inside the mesh's indices; the software backend loops over them only, clipped to the
         * mesh.
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
         * - Vulkan: as OpenGL (a port of its shader): antialiased over one screen pixel and blended, depth as the
         *   draw's state says, outline and shadow composited under the face in one draw, softness widening the ramp;
         *   null only in a build without the built-in shaders (no SPIR-V compiler found).
         * - The default body: null (a renderer or fake without a text shader).
         * Effect uniforms (uOutline, uOutlineColor, uSoftness, uShadowColor, uShadowOffset, uShadowSoftness;
         * see TextDrawing::EffectUniforms in the engine) are all 0 for plain text. OpenGL composites the
         * outline and shadow under the face; the software renderer alpha-tests them and ignores softness.
         */
        [[nodiscard]] virtual IShader* GetStandardTextShader() const { return nullptr; }

        /**
         * Copies the last frame into `buffer` (width * height * 4 bytes): RGBA8, rows bottom to top (row 0 is
         * the bottom of the image), as glReadPixels returns them. Every backend gives this same layout; what
         * it reads differs:
         * - OpenGL: glReadPixels of the bottom-left width x height region of the frame's target (the offscreen
         *   target while SetRenderTargetSize is in effect, otherwise the window's back buffer).
         * - Software: the CPU colour buffer, waiting for the frame in flight first, resampled (nearest) to
         *   width x height (a plain copy when that is the renderer's size).
         * - Vulkan: the offscreen colour image of the last frame (all black before the first), resampled
         *   (nearest) to width x height when that is not the target's size; writes nothing when not initialized.
         * Callers that want the top row first (the editor's FrameData) flip it with FlipRows (FrameRows.hpp).
         */
        virtual void ReadFramebuffer(std::uint8_t *buffer, int width, int height) const = 0;

        /**
         * Renders every later frame at width x height pixels, whatever the window's size, for ReadFramebuffer
         * to read back at that size (the editor's viewport, #69). Returns true when frames will render at that
         * size. False for a zero dimension (nothing changes) or when the backend couldn't make a target of that
         * size; calling it again with the same size then tries again. Per backend:
         * - OpenGL: frames go to an offscreen framebuffer of that size (RGBA8 colour, D24S8 depth and stencil)
         *   instead of the window, so the window shows nothing new. Resize and OnResize only set the window's
         *   size, used while there is no offscreen target (Window::SetRenderSize stops forwarding window resizes
         *   anyway). The same size again keeps the target; another size replaces it. False when the framebuffer
         *   is incomplete: it logs, the old target is gone, and frames render to the window.
         * - Software and the default body: Resize(width, height), since the frame is already offscreen (the CPU
         *   colour buffer); true for any non-zero size. A later Resize or OnResize changes it again, so a host
         *   that sets this stops forwarding window resizes (Window::SetRenderSize does).
         * - Vulkan: recreates its offscreen colour and depth targets at that size (it always renders offscreen);
         *   Resize and OnResize are then ignored. False over the device's size limit or when allocation fails (the
         *   old target is gone; frames are skipped until a call succeeds).
         */
        [[nodiscard]] virtual bool SetRenderTargetSize(uint32_t width, uint32_t height)
        {
            if (width == 0 || height == 0)
            {
                return false;
            }
            Resize(width, height);
            return true;
        }

        // Debug
        virtual void SetWireframe(bool enabled) = 0;
        [[nodiscard]] virtual const char* GetRendererName() const = 0;

        /**
         * The editor's prefab stage (In Scene mode): every draw recorded from now until the next call is dimmed by at
         * least `dim` (RenderState::dim; the larger of the draw's own and this), clamped to 0..1. A frame-scoped
         * modifier that each backend reads when a draw is RECORDED (DrawMesh, DrawObjects) and stores in that draw's
         * state, so it still travels with the draw when a backend reorders draws (the software renderer does). 0 (the
         * default) dims nothing; the stage sets it for the scene drawn behind the prefab, then back to 0.
         */
        void SetStageDim(const float dim) { m_stageDim = std::clamp(dim, 0.0f, 1.0f); }
        [[nodiscard]] float GetStageDim() const { return m_stageDim; }
        /// `state` as a draw recorded now carries it: its dim raised to SetStageDim's value
        [[nodiscard]] RenderState WithStageDim(const RenderState &state) const
        {
            RenderState applied = state;
            applied.dim = std::max(std::clamp(state.dim, 0.0f, 1.0f), m_stageDim);
            return applied;
        }
        /// Whether this backend's built-in shaders apply RenderState::dim. The four backends do (OpenGL, Vulkan, WebGPU,
        /// software); the default (a test fake) does not.
        [[nodiscard]] virtual bool SupportsStageDim() const { return false; }
        /// Whether blended draws (RenderState::blend) are blended. The software renderer draws them opaque.
        [[nodiscard]] virtual bool SupportsBlending() const { return true; }

        /**
         * The screen tint (ScreenTint: a saturation inside or outside a circle on the screen; effects design, stage E2):
         * every draw recorded from now until the next call carries `tint` in its RenderState::tint, read when the draw is
         * RECORDED like the stage dim, so it still travels with the draw when a backend reorders draws. Application sets
         * it for the scene pass only and clears it before the UI pass. Taken through ScreenTint::Sanitized (saturation
         * clamped to 0..1, a negative radius to 0, a value that is not finite: no tint). The default (ScreenTint{})
         * tints nothing.
         */
        void SetScreenTint(const ScreenTint &tint) { m_screenTint = ScreenTint::Sanitized(tint); }
        [[nodiscard]] const ScreenTint& GetScreenTint() const { return m_screenTint; }
        /// `state` as a draw recorded now carries it: WithStageDim, and the screen tint when one is set (a draw's own
        /// tint stays when none is, taken through ScreenTint::Sanitized like SetScreenTint's, since the backends hand it
        /// to the shaders as it is)
        [[nodiscard]] RenderState WithFrameModifiers(const RenderState &state) const
        {
            RenderState applied = WithStageDim(state);
            applied.tint = m_screenTint.IsActive() ? m_screenTint : ScreenTint::Sanitized(state.tint);
            return applied;
        }
        /// Whether this backend's built-in shaders apply RenderState::tint. The four backends do (OpenGL, Vulkan, WebGPU,
        /// software); the default (a test fake) does not.
        [[nodiscard]] virtual bool SupportsScreenTint() const { return false; }

        /**
         * What the last finished frame drew (EN5): its draw calls, triangles and vertices, counted between its BeginFrame
         * and EndFrame (RenderCounters). Like the stage dim, the counting is non-virtual state each backend updates where
         * it records a draw (NoteDraw), clears in BeginFrame (ResetRenderCounters) and publishes in EndFrame
         * (PublishRenderCounters). All zeros before the first frame, and for a backend that does not count.
         */
        [[nodiscard]] const RenderCounters& GetLastFrameCounters() const { return m_lastFrameCounters; }
        /// Whether this backend counts its draws (GetLastFrameCounters means something). The four backends do (OpenGL,
        /// Vulkan, WebGPU, software); the default (a test fake) does not.
        [[nodiscard]] virtual bool SupportsRenderCounters() const { return false; }

        /**
         * General-purpose compute on this renderer's device (GpuCompute.hpp), or nullptr for a backend that has none (OpenGL,
         * Vulkan and software today; the WebGPU renderer has it while initialized). The object belongs to the renderer:
         * hold it next to GetLifetimeToken and stop calling it once the token has expired.
         */
        [[nodiscard]] virtual IGpuCompute* GetCompute() { return nullptr; }

    protected:
        /// Expires every token handed out so far: the renderer's resources are gone (Shutdown). Later calls to
        /// GetLifetimeToken return a fresh token.
        void EndLifetime() { m_lifetime = std::make_shared<const int>(0); }

        /// A frame starts: nothing drawn in it yet
        void ResetRenderCounters()
        {
            m_frameCounters.drawCalls = 0;
            m_frameCounters.triangles = 0;
            m_frameCounters.vertices = 0;
        }
        /// One draw of `indexCount` indices of a mesh of `vertexCount` vertices reached the backend's draw call
        void NoteDraw(const std::uint32_t indexCount, const std::uint32_t vertexCount)
        {
            ++m_frameCounters.drawCalls;
            m_frameCounters.triangles += indexCount / 3;
            m_frameCounters.vertices += vertexCount;
        }
        /// The frame is finished: its counts become the last frame's
        void PublishRenderCounters()
        {
            ++m_frameCounters.frames;
            m_lastFrameCounters = m_frameCounters;
        }

    private:
        std::shared_ptr<const int> m_lifetime = std::make_shared<const int>(0);
        float m_stageDim = 0.0f;
        ScreenTint m_screenTint{};
        RenderCounters m_frameCounters{};
        RenderCounters m_lastFrameCounters{};
    };
}
