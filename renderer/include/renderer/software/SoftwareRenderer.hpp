#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include <memory>

#include "renderer/common/Renderer.hpp"
#include "renderer/software/RenderThread.hpp"
#include "renderer/software/SWMesh.hpp"
#include "renderer/software/SWTexture.hpp"
#include "renderer/software/SWShader.hpp"
#include "renderer/software/SWMaterial.hpp"

namespace Renderer::Software
{
    /**
     * Everything rasterizing a draw reads from its material, copied by DrawMesh on the main thread. The
     * render thread shades from this copy, never from the SWMaterial, so a material changed after DrawMesh
     * only affects later draws (as OpenGL applies uniforms at draw submission), and changing or destroying
     * a material never races the frame in flight.
     */
    struct SWMaterialSnapshot
    {
        std::array<float, 4> albedo{1.0f, 1.0f, 1.0f, 1.0f}; ///< uAlbedo
        float smoothness = 0.5f;                             ///< uSmoothness (lit only)
        /// uAlphaCutoff (unlit and lit): a pixel whose final alpha is below it is skipped, as OpenGL discards
        /// it. The default, 0, skips nothing.
        float alphaCutoff = 0.0f;
        /// Text effects (text shader only; all 0 = none). uOutline: the outline's outer edge is at distance
        /// 0.5 - outline. uOutlineColor: its colour. uShadowColor: the shadow's colour (alpha 0 = no shadow).
        /// uShadowOffset: the shadow's uv displacement (the shadow at uv is the shape at uv - offset). The
        /// softness uniforms aren't copied: with no blending, the software text shader ignores softness.
        float textOutline = 0.0f;
        std::array<float, 4> textOutlineColor{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> textShadowColor{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 2> textShadowOffset{0.0f, 0.0f};
        /// The material's texture, or null when it has none or it is empty. Still a pointer: its pixels are
        /// read at raster time, which is safe because DestroyTexture waits for the frame in flight and a
        /// software texture's pixels never change after creation.
        const SWTexture *texture = nullptr;
        /// Lit only. uEmissive (rgb; added after lighting) and uOcclusionStrength. The emissive texture multiplies
        /// uEmissive; the occlusion texture's red channel scales the ambient light. Textures are pointers like
        /// `texture`, null when absent. A base colour or emissive texture made with TextureOptions::srgb is
        /// decoded through a table in linear lighting.
        std::array<float, 4> emissive{0.0f, 0.0f, 0.0f, 0.0f};
        float occlusionStrength = 1.0f;
        const SWTexture *emissiveTexture = nullptr;
        const SWTexture *occlusionTexture = nullptr;
        /// Lit only. The normal map (a tangent-space normal in rgb, scaled by uNormalScale: x and y are multiplied by
        /// it) and, with uPbr, the metallic-roughness texture (green roughness, blue metallic). Data textures:
        /// read as plain bytes, never sRGB.
        const SWTexture *normalTexture = nullptr;
        float normalScale = 1.0f;
        const SWTexture *metallicRoughnessTexture = nullptr;
        /// Lit only: uPbr (metallic-roughness PBR instead of Blinn-Phong) and uMetallic
        bool pbr = false;
        float metallic = 0.0f;
        SWShaderType shader = SWShaderType::Unlit; ///< Unlit too when the material's shader isn't an SWShader
    };

    /**
     * A CPU rasterizer. Draws are recorded between BeginFrame and EndFrame, and EndFrame hands the frame to
     * a render thread that rasterizes it into a CPU colour and depth buffer. Present waits for that frame,
     * then (with a window) uploads the colour buffer to a GL texture and draws it over the window.
     *
     * Headless mode: Initialize(nullptr, width, height) makes no GL calls at all (no context, no glad, no
     * blit texture). Frames still rasterize on the render thread; Present only waits for the frame, and
     * ReadFramebuffer returns the CPU colour buffer. It needs no window or GPU, so tests render real pixels
     * with it in CI.
     */
    class SoftwareRenderer : public Common::IRenderer
    {
    public:
        SoftwareRenderer() = default;
        /// Stops the render thread (if Shutdown hasn't) before any resource it might be reading is freed
        ~SoftwareRenderer() override;
        SoftwareRenderer(const SoftwareRenderer &) = delete;
        SoftwareRenderer &operator=(const SoftwareRenderer &) = delete;

        // Lifecycle
        /// With a window: makes its context current, loads GL (false if that fails) and creates the blit
        /// resources. With windowHandle null: headless (see the class comment); always succeeds.
        bool Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height) override;
        /// Stops the render thread and frees every resource, and the GL blit objects if they were created.
        /// Safe headless, without Initialize, and twice.
        void Shutdown() override;
        /// Waits for a frame still rasterizing before reallocating the buffers
        void Resize(uint32_t width, uint32_t height) override;
        void Clear(float r, float g, float b, float a) override;

        // Frame
        void BeginFrame() override;
        void EndFrame() override;
        /// Waits for the frame EndFrame submitted, then blits it to the window. Headless it only waits.
        void Present() override;

        // Shaders
        Common::IShader* CreateShaderProgram(const char *vs, const char *fs) override;
        void UseShaderProgram(Common::IShader *shader) override;
        /// The built-in shaders are never freed. No wait is needed: draws keep a copy of the shader type.
        bool DestroyShaderProgram(Common::IShader *shader) override;
        bool IsValidShader(Common::IShader *shader) const override;

        // Resources
        Common::IMesh* CreateMesh(const Common::MeshData &meshData) override;
        /// Waits for any frame still rasterizing (which may draw the mesh) before freeing it
        void DestroyMesh(Common::IMesh *mesh) override;
        Common::ITexture* CreateTexture(const uint8_t *data, uint32_t w, uint32_t h, uint32_t ch) override;
        /// Stores the options on the SWTexture. The unlit and lit shaders honour wrap and filter (bilinear for
        /// Linear, nearest for Nearest); there are no mipmaps, so mipmaps has no visible effect on this backend.
        /// The text shader always samples bilinearly.
        Common::ITexture* CreateTexture(const uint8_t *data, uint32_t w, uint32_t h, uint32_t ch,
                                        const Common::TextureOptions &options) override;
        /// Waits for any frame still rasterizing (which may sample the texture) before freeing it
        void DestroyTexture(Common::ITexture *texture) override;
        /// Waits for any frame still rasterizing (which may read the mesh) before replacing its data
        bool UpdateMesh(Common::IMesh *mesh, const Common::MeshData &meshData) override;
        Common::IMaterial* CreateMaterial(Common::IShader *shader) override;
        Common::IMaterial* CreateMaterial(Common::IShader *shader, Common::ITexture *texture) override;
        /// Frees the material at once: the frame in flight shades from copies (SWMaterialSnapshot)
        void DestroyMaterial(Common::IMaterial *material) override;

        // Rendering
        void SetViewProjection(const float *view, const float *projection) override;
        void UpdateSceneLighting(const Common::SceneLightingData &lighting,
                                 const N2Engine::Math::Vector3 &cameraPosition) override;
        using Common::IRenderer::DrawMesh; // the default-state overload
        /// Records the draw, with a copy of what it reads from the material (SWMaterialSnapshot), so later
        /// changes to the material don't affect it, as on OpenGL. EndFrame rasterizes the frame's draws in
        /// the order OrderDraws gives (DrawOrder.hpp). depthTest, depthWrite and cull are honoured per draw; blend is ignored, as
        /// this renderer has no blending (a Transparent-queue draw is drawn opaque, without writing depth).
        /// One exception: a draw with the text shader alpha-tests, and writes depth wherever it covers a
        /// pixel and depthTest is on, whatever depthWrite says (see SWShaderType::Text).
        void DrawMesh(Common::IMesh *mesh, const float *modelMatrix, Common::IMaterial *material,
                      const Common::RenderState &state) override;
        /// As DrawMesh above, rasterizing only the triangles in `range` (a submesh). The range is checked against
        /// the mesh when the frame rasterizes: the part outside the mesh's indices, and a last incomplete
        /// triangle, are skipped.
        void DrawMesh(Common::IMesh *mesh, const float *modelMatrix, Common::IMaterial *material,
                      const Common::RenderState &state, const Common::IndexRange &range) override;
        void DrawObjects(const std::vector<Common::RenderObject> &objects) override;
        void OnResize(int width, int height) override;

        // The built-in shaders exist from construction, so they are valid before Initialize too
        Common::IShader* GetStandardUnlitShader() const override { return m_unlitShader.get(); }
        Common::IShader* GetStandardLitShader() const override { return m_litShader.get(); }
        /// The alpha-tested SDF text shader (SWShaderType::Text)
        Common::IShader* GetStandardTextShader() const override { return m_textShader.get(); }

        /**
         * Waits for the frame in flight, then copies the colour buffer into `buffer` as RGBA8, 4 bytes per
         * pixel, rows bottom to top (row 0 is the bottom of the image, as glReadPixels gives). A size other
         * than the renderer's is resampled, nearest (OpenGL's reads a region instead, not resampled; the
         * layout is the same). SetRenderTargetSize (the default body: Resize) makes the renderer's size the
         * one read, so the copy is exact.
         */
        void ReadFramebuffer(uint8_t *buffer, int width, int height) const override;

        void SetWireframe(bool enabled) override;
        const char* GetRendererName() const override { return "Software Rasterizer"; }

    private:
        struct DrawCommand {
            SWMesh* mesh;
            float modelMatrix[16];
            SWMaterialSnapshot material; // copied at DrawMesh: the render thread never reads the SWMaterial
            Common::RenderState state;
            // The view and projection set when the draw was submitted, so a SetViewProjection later in the
            // frame (the UI pass) only affects the draws after it, as in OpenGL
            float view[16];
            float proj[16];
            // The indices to rasterize (DrawMesh with an IndexRange); the whole mesh by default, as it is when
            // the frame rasterizes (an UpdateMesh later in the frame still draws in full)
            uint32_t firstIndex = 0;
            uint32_t indexCount = std::numeric_limits<uint32_t>::max();
        };

        std::vector<DrawCommand> m_drawQueue;
        // mutable: ReadFramebuffer (const) waits for the frame in flight
        mutable RenderThread m_renderThread;

        // Framebuffer
        uint32_t m_width = 0, m_height = 0;
        std::vector<uint32_t> m_colorBuffer; // RGBA8 packed
        std::vector<float> m_depthBuffer;
        float m_clearR = 0, m_clearG = 0, m_clearB = 0, m_clearA = 1;

        // GL blit. None of it exists headless (m_glReady false), and then no GL function is ever called.
        GLFWwindow *m_window = nullptr;
        bool m_glReady = false;
        unsigned int m_blitTex = 0, m_blitVAO = 0, m_blitVBO = 0, m_blitProg = 0;
        bool SetupBlitResources();

        // Matrices (row-major, matching OpenGLRenderer convention). Main thread only: each draw copies them.
        float m_view[16]{}, m_proj[16]{};

        // Lighting state
        Common::SceneLightingData m_lighting;
        N2Engine::Math::Vector3 m_cameraPos{};

        // Built-in shaders
        std::unique_ptr<SWShader> m_unlitShader = std::make_unique<SWShader>(SWShaderType::Unlit);
        std::unique_ptr<SWShader> m_litShader = std::make_unique<SWShader>(SWShaderType::Lit);
        std::unique_ptr<SWShader> m_textShader = std::make_unique<SWShader>(SWShaderType::Text);

        // Owned resource sets (for lifetime tracking)
        std::vector<std::unique_ptr<SWMesh>> m_meshes;
        std::vector<std::unique_ptr<SWTexture>> m_textures;
        std::vector<std::unique_ptr<SWMaterial>> m_materials;
        std::vector<std::unique_ptr<SWShader>> m_shaders;

        bool m_wireframe = false;

        // Rasterizer internals
        // Render thread: everything per frame comes in as arguments (the EndFrame snapshot), so the render
        // thread never writes members the main thread also writes (Clear, UpdateSceneLighting)
        void ClearBuffers(float r, float g, float b, float a);
        void SetPixel(int x, int y, float depth, uint32_t color);

        struct SWFragment
        {
            float x, y, z;        // NDC after persp divide
            float wx, wy, wz;     // world-space position (for lighting)
            float nx, ny, nz;     // interpolated normal (world)
            float u, v;           // interpolated texcoord
            float r, g, b, a;     // interpolated vertex color
        };

        void RasterizeMesh(SWMesh* mesh, const float* modelMatrix, const float* view, const float* proj,
                           const SWMaterialSnapshot& material, const Common::RenderState& state,
                           uint32_t firstIndex, uint32_t indexCount,
                           const Common::SceneLightingData& lighting, const N2Engine::Math::Vector3& cameraPos);
        /// Records a draw of `indexCount` indices from `firstIndex` (both DrawMesh overloads)
        void RecordDraw(Common::IMesh *mesh, const float *modelMatrix, Common::IMaterial *material,
                        const Common::RenderState &state, uint32_t firstIndex, uint32_t indexCount);


        // Math helpers (row-major)
        void Mul4x4(const float *a, const float *b, float *out) const;
        void TransformPoint(const float *mat, const float *in3, float *out4) const;
        void TransformNormal(const float *modelMatrix, const float *n, float *out) const;
    };
}