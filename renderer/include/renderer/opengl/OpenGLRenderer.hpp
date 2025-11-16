// renderer/opengl/OpenGLRenderer.hpp
#pragma once

#include "renderer/common/Renderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLMaterial.hpp" // Add this

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
        virtual ~OpenGLRenderer();

        // Lifecycle
        bool Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height) override;
        void Shutdown() override;
        void Resize(uint32_t width, uint32_t height) override;
        void Clear(float r, float g, float b, float a) override;

        // Shader management
        uint32_t CreateShaderProgram(const char *vertexSource, const char *fragmentSource) override;
        void UseShaderProgram(uint32_t shaderId) override;
        void DestroyShaderProgram(uint32_t shaderId) override;
        bool IsValidShader(uint32_t shaderId) const override;

        // Frame management
        void BeginFrame() override;
        void EndFrame() override;
        void Present() override;

        // Resource management
        uint32_t CreateMesh(const Common::MeshData &meshData) override;
        void DestroyMesh(uint32_t meshId) override;
        uint32_t CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels) override;
        void DestroyTexture(uint32_t textureId) override;

        // Updated material management
        Common::IMaterial *CreateMaterial(uint32_t shaderId, uint32_t textureId = 0) override;
        void DestroyMaterial(Common::IMaterial *material) override;

        // Rendering - updated signature
        void SetViewProjection(const float *view, const float *projection) override;
        void DrawMesh(uint32_t meshId, const float *modelMatrix, Common::IMaterial *material) override;
        void DrawObjects(const std::vector<Common::RenderObject> &objects) override;
        void OnResize(int width, int height) override;

        // Debug
        void SetWireframe(bool enabled) override;
        const char *GetRendererName() const override;

    private:
        GLFWwindow *m_window;
        uint32_t m_width;
        uint32_t m_height;

        // Shader storage - use shared_ptr so materials can share
        std::unordered_map<uint32_t, std::shared_ptr<OpenGLShader>> m_shaderPrograms;
        uint32_t m_nextShaderId;
        uint32_t m_currentShader;

        // View/Projection matrices
        float m_viewMatrix[16];
        float m_projectionMatrix[16];

        // Resource containers
        std::unordered_map<uint32_t, Mesh> m_meshes;
        std::unordered_map<uint32_t, Texture> m_textures;

        // Materials - store as unique_ptr, return raw pointers
        std::vector<std::unique_ptr<OpenGLMaterial>> m_materials;

        // Resource ID counters
        uint32_t m_nextMeshId;
        uint32_t m_nextTextureId;

        // State
        bool m_wireframeEnabled;
        float m_clearColor[4];

        // Helper methods
        GLuint CompileShader(const char *source, GLenum shaderType);
        GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader);
        bool CheckCompileErrors(GLuint shader, const std::string &type);
        void SetMatrix4fv(GLint location, const float *matrix);
        GLenum GetOpenGLFormat(uint32_t channels);
        GLenum GetOpenGLInternalFormat(uint32_t channels);
    };

    std::unique_ptr<Common::IRenderer> CreateOpenGLRenderer();
}