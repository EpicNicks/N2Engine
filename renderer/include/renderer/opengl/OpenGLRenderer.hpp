#pragma once

#include "renderer/common/Renderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLMaterial.hpp"
#include "renderer/opengl/OpenGLMesh.hpp"
#include "renderer/opengl/OpenGLTexture.hpp"

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
        void DestroyTexture(Common::ITexture* texture) override;

        // Updated material management
        Common::IMaterial* CreateMaterial(Common::IShader* shader, Common::ITexture* texture = nullptr) override;
        void DestroyMaterial(Common::IMaterial* material) override;

        // Rendering - updated signature
        void SetViewProjection(const float* view, const float* projection) override;
        void UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                 const N2Engine::Math::Vector3& cameraPosition) override;
        void DrawMesh(Common::IMesh* meshId, const float* modelMatrix, Common::IMaterial* material) override;
        void DrawObjects(const std::vector<Common::RenderObject>& objects) override;
        void OnResize(int width, int height) override;

        // Debug
        void SetWireframe(bool enabled) override;
        [[nodiscard]] const char* GetRendererName() const override;

        [[nodiscard]] Common::IShader* GetStandardUnlitShader() const override;
        [[nodiscard]] Common::IShader* GetStandardLitShader() const override;

    private:
        GLFWwindow* m_window;
        uint32_t m_width;
        uint32_t m_height;

        Common::IShader* m_standardUnlitShader;
        Common::IShader* m_standardLitShader;

        // View/Projection matrices
        float m_viewMatrix[16]{};
        float m_projectionMatrix[16]{};

        uint32_t m_currentShader;

        // resource containers
        std::unordered_map<Common::IShader*, std::shared_ptr<OpenGLShader>> m_shaderPrograms;
        std::unordered_map<Common::IMesh*, std::unique_ptr<OpenGLMesh>> m_meshes;
        std::unordered_map<Common::ITexture*, std::unique_ptr<OpenGLTexture>> m_textures;
        std::unordered_map<Common::IMaterial*, std::unique_ptr<OpenGLMaterial>> m_materials;

        // State
        bool m_wireframeEnabled;
        float m_clearColor[4];

        // Helper methods
        GLuint CompileShader(const char* source, GLenum shaderType);
        GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader);
        bool CheckCompileErrors(GLuint shader, const std::string& type);
        void SetMatrix4fv(GLint location, const float* matrix);
        GLenum GetOpenGLFormat(uint32_t channels);
        GLenum GetOpenGLInternalFormat(uint32_t channels);

        void CreateStandardShaders();
    };

    std::unique_ptr<Common::IRenderer> CreateOpenGLRenderer();
}
