#pragma once

#include "renderer/common/Renderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLMaterial.hpp"
#include "renderer/opengl/OpenGLMesh.hpp"
#include "renderer/opengl/OpenGLTexture.hpp"

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
        bool Initialize(GLFWwindow* windowHandle, uint32_t width, uint32_t height) override;
        void Shutdown() override;
        void Resize(uint32_t width, uint32_t height) override;
        void Clear(float r, float g, float b, float a) override;

        // Shader management
        Common::IShader* CreateShaderProgram(const char* vertexSource, const char* fragmentSource) override;
        void UseShaderProgram(Common::IShader* shader) override;
        bool DestroyShaderProgram(Common::IShader* shader) override;
        bool IsValidShader(Common::IShader* shader) const override;

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

        [[nodiscard]] Common::IShader* GetStandardUnlitShader() const override;
        [[nodiscard]] Common::IShader* GetStandardLitShader() const override;
        [[nodiscard]] Common::IShader* GetStandardTextShader() const override;

        /// RGBA, bottom row first, from the offscreen target while there is one, else the window's back buffer
        void ReadFramebuffer(std::uint8_t *buffer, int width, int height) const override;
        /// Renders to an offscreen framebuffer of this size from the next BeginFrame (see IRenderer)
        void SetRenderTargetSize(uint32_t width, uint32_t height) override;

    private:
        GLFWwindow* m_window;
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
