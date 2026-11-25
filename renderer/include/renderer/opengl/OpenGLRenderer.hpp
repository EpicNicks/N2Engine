#pragma once

#include "renderer/common/BlockMaterial.hpp"
#include "renderer/common/Renderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLMaterial.hpp"
#include "renderer/opengl/OpenGLMesh.hpp"
#include "renderer/opengl/OpenGLTexture.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

namespace Renderer::OpenGL
{
    struct Mesh
    {
        GLuint VAO;
        GLuint VBO;
        GLuint EBO;
        uint32_t indexCount;
        bool isValid;
    };

    struct Texture
    {
        GLuint handle;
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        bool isValid;
    };

    class OpenGLRenderer : public Common::IRenderer
    {
    public:
        OpenGLRenderer();
        ~OpenGLRenderer() override;

        // Lifecycle
        bool Initialize(const Common::Surface &surface, uint32_t width, uint32_t height) override;
        void Shutdown() override;
        void Resize(uint32_t width, uint32_t height) override;
        void Clear(float r, float g, float b, float a) override;

        // Shader management
        Common::IShader* CreateShaderProgram(const char* vertexSource, const char* fragmentSource) override;
        void UseShaderProgram(Common::IShader* shader) override;
        bool DestroyShaderProgram(Common::IShader* shader) override;
        bool IsValidShader(Common::IShader* shader) const override;
        /// User shaders (stage 4 of orchestrator/proposals/user-shaders.md): the driver compiles the cooked GLSL 330 text of
        /// `blob` (fragmentGlsl) with the engine's vertex stage (UserGl::VertexSource). The per-draw uniforms (n2_Model ...,
        /// n2_TargetHeight) and, when the manifest asks for it, the scene lighting are set by name, as for the built-ins. Null,
        /// logged, for a blob ValidateUserShaderBlob rejects for OpenGL, past kMaxUserShaders, or a program the driver refuses
        Common::IShader* CreateUserShader(const Common::UserShaderBlob& blob) override;
        Common::IMaterial* CreateUserMaterial(Common::IShader* shader, Common::ITexture* texture) override;
        [[nodiscard]] std::optional<Common::UserShaderTarget> GetUserShaderTarget() const override { return Common::UserShaderTarget::OpenGl; }

        // Frame management
        void BeginFrame() override;
        void EndFrame() override;
        void Present() override;

        // Resource management
        Common::IMesh* CreateMesh(const Common::MeshData& meshData) override;
        void DestroyMesh(Common::IMesh* mesh) override;
        Common::ITexture*
        CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels) override;
        Common::ITexture* CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels,
                                        const Common::TextureOptions& options) override;
        void DestroyTexture(Common::ITexture* texture) override;
        bool UpdateMesh(Common::IMesh* mesh, const Common::MeshData& meshData) override;

        // Updated material management
        Common::IMaterial* CreateMaterial(Common::IShader* shader) override;
        Common::IMaterial* CreateMaterial(Common::IShader* shader, Common::ITexture* texture) override;
        void DestroyMaterial(Common::IMaterial* material) override;

        // Rendering - updated signature
        void SetViewProjection(const float* view, const float* projection) override;
        void UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                 const N2Engine::Math::Vector3& cameraPosition) override;
        using Common::IRenderer::DrawMesh; // the default-state overload
        void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                      const Common::RenderState& state) override;
        /// glDrawElements over the range only (an offset into the index buffer). A range that isn't inside the
        /// mesh's indices draws nothing.
        void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                      const Common::RenderState& state, const Common::IndexRange& range) override;
        void DrawObjects(const std::vector<Common::RenderObject>& objects) override;
        void OnResize(int width, int height) override;

        // Debug
        void SetWireframe(bool enabled) override;
        [[nodiscard]] const char* GetRendererName() const override;
        /// The built-in shaders take uStageDim (RenderState::dim)
        [[nodiscard]] bool SupportsStageDim() const override { return true; }
        /// The built-in shaders take uTintSaturation, uTintInside and uTintCircle (RenderState::tint) and test gl_FragCoord
        [[nodiscard]] bool SupportsScreenTint() const override { return true; }
        /// Counts each glDrawElements of DrawIndices
        [[nodiscard]] bool SupportsRenderCounters() const override { return true; }

        [[nodiscard]] Common::IShader* GetStandardUnlitShader() const override;
        [[nodiscard]] Common::IShader* GetStandardLitShader() const override;
        [[nodiscard]] Common::IShader* GetStandardTextShader() const override;

        /// RGBA, bottom row first, from the offscreen target while there is one, else the window's back buffer
        void ReadFramebuffer(std::uint8_t *buffer, int width, int height) const override;
        /// Renders to an offscreen framebuffer of this size from the next BeginFrame (see IRenderer); false, with
        /// no target, when the framebuffer is incomplete
        [[nodiscard]] bool SetRenderTargetSize(uint32_t width, uint32_t height) override;

        /// Post-processing and render textures (issue #445): the frame's target is blitted to a texture and the passes run with
        /// one GLSL 330 program over a full-screen triangle; captures are a scaled glBlitFramebuffer
        [[nodiscard]] bool SupportsPostProcess() const override { return true; }
        void ApplyPostProcess(const Common::PostProcessSettings& settings) override;
        /// A texture whose alpha reads as 1 (GL_TEXTURE_SWIZZLE_A), so a capture is opaque
        Common::ITexture* CreateRenderTexture(uint32_t width, uint32_t height) override;
        bool CaptureFrame(Common::ITexture* target) override;

    private:
        // Post-processing (OpenGLPostProcess.cpp): the program, an empty vertex array, the copy of the frame and the two
        // half-size bloom textures, each with a framebuffer
        struct PostTarget
        {
            GLuint texture = 0;
            GLuint framebuffer = 0;
        };
        GLuint m_postProgram = 0;
        GLuint m_postVertexArray = 0;
        bool m_postProgramFailed = false;
        PostTarget m_postScene{};
        PostTarget m_postBloomA{};
        PostTarget m_postBloomB{};
        uint32_t m_postWidth = 0;
        uint32_t m_postHeight = 0;
        std::vector<Common::ITexture*> m_renderTextures;
        bool EnsurePostResources(uint32_t width, uint32_t height);
        void DestroyPostTargets();
        void DestroyPostResources();
        /// The framebuffer the frame draws to (0: the window) and its size
        [[nodiscard]] GLuint FrameTarget(uint32_t& width, uint32_t& height) const;
        /// Every draw after this re-applies its state, program and wireframe mode (the post passes changed them)
        void RestoreFrameState(GLuint target, uint32_t width, uint32_t height);

        /// The window and its OpenGL context operations (the platform's)
        Common::Surface m_window;
        uint32_t m_width;
        uint32_t m_height;

        // The offscreen target SetRenderTargetSize creates; 0 (and the window's framebuffer used) until then
        GLuint m_offscreenFramebuffer = 0;
        GLuint m_offscreenColor = 0;
        GLuint m_offscreenDepth = 0;
        uint32_t m_offscreenWidth = 0;
        uint32_t m_offscreenHeight = 0;

        Common::IShader* m_standardUnlitShader;
        Common::IShader* m_standardLitShader;
        Common::IShader* m_standardTextShader = nullptr;

        // View/Projection matrices
        float m_viewMatrix[16]{};
        float m_projectionMatrix[16]{};

        Common::SceneLightingData m_currentLighting;
        N2Engine::Math::Vector3 m_cameraPosition{0.0f, 0.0f, 0.0f}; // the last UpdateSceneLighting's, for user shaders made later

        /// What a user shader (a program in m_shaderPrograms) needs besides the program: its material layout, whether it reads the
        /// scene lighting, and its name for log lines
        struct UserShaderInfo
        {
            std::shared_ptr<const Gpu::MaterialLayout> layout;
            bool usesLighting = false;
            std::string label;
        };
        std::unordered_map<Common::IShader*, UserShaderInfo> m_userShaders;
        /// The 1x1 textures an unset slot reads (white, black, flat normal, transparent), made with the first user shader
        std::array<OpenGLTexture*, 4> m_userTextureDefaults{};
        void ApplyLightingToUserShaders();
        void EnsureUserTextureDefaults();
        void BindUserTextures(const OpenGLMaterial& material);

        // Linear lighting: GL_FRAMEBUFFER_SRGB is on only while the lit shader draws (see DrawIndices), and only
        // when this frame's target stores sRGB (the window's, if it was made sRGB-capable, and the offscreen
        // target); otherwise the lit shader encodes its own output
        bool m_framebufferIsSrgb = false; // the current target's, chosen at BeginFrame from the two below
        bool m_framebufferSrgbOn = false;
        // Whether each target stores sRGB, asked once when the target is made (Initialize, SetRenderTargetSize)
        bool m_windowIsSrgb = false;
        bool m_offscreenIsSrgb = false;
        // The standard lit shader's program object, to tell its draws from the others'
        const void *m_litShaderImpl = nullptr;

        uint32_t m_currentShader;

        // resource containers
        std::unordered_map<Common::IShader*, std::shared_ptr<OpenGLShader>> m_shaderPrograms;
        std::unordered_map<Common::IMesh*, std::unique_ptr<OpenGLMesh>> m_meshes;
        std::unordered_map<Common::ITexture*, std::unique_ptr<OpenGLTexture>> m_textures;
        std::unordered_map<Common::IMaterial*, std::unique_ptr<OpenGLMaterial>> m_materials;

        // State
        // The fixed-function state last set on the GL context, so a draw only changes what differs.
        // Invalid until BeginFrame sets every field (anything may have touched GL state in between).
        Common::RenderState m_appliedState{};
        bool m_appliedStateValid = false;
        bool m_wireframeEnabled;
        float m_clearColor[4];

        // Helper methods
        GLuint CompileShader(const char* source, GLenum shaderType);
        GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader);
        bool CheckCompileErrors(GLuint shader, const std::string& type);
        static void SetMatrix4fv(GLint location, const float* matrix);
        static GLenum GetOpenGLFormat(uint32_t channels);
        static GLenum GetOpenGLInternalFormat(uint32_t channels);

        void CreateStandardShaders();
        void DestroyOffscreenTarget();
        void ApplyRenderState(const Common::RenderState& state);
        /// Both DrawMesh overloads: `indexCount` indices from `firstIndex`, already checked against the mesh
        void DrawIndices(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                         const Common::RenderState& state, std::uint32_t firstIndex, std::uint32_t indexCount);
    };

    std::unique_ptr<Common::IRenderer> CreateOpenGLRenderer();
}
