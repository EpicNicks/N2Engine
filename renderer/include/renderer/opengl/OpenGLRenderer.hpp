#pragma once

#include "renderer/common/Renderer.hpp"

#include <unordered_map>
#include <string>

namespace Renderer
{
    namespace OpenGL
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

        struct Material
        {
            uint32_t shaderId;  // Changed from textureId to shaderId
            uint32_t textureId; // Keep texture support
            bool isValid;
        };

        struct ShaderUniforms
        {
            GLint modelLoc = -1;
            GLint viewLoc = -1;
            GLint projectionLoc = -1;
            GLint textureLoc = -1;
            // Add more uniforms as needed
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

            // Multi-shader management (new interface)
            uint32_t CreateShaderProgram(const char *vertexSource, const char *fragmentSource) override;
            uint32_t LoadShaderProgram(const std::string &vertexPath, const std::string &fragmentPath) override;
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
            uint32_t CreateMaterial(uint32_t shaderId, uint32_t textureId = 0) override;
            void DestroyMaterial(uint32_t materialId) override;

            // Rendering
            void SetViewProjection(const float *view, const float *projection) override;
            void DrawMesh(uint32_t meshId, const float *modelMatrix, uint32_t materialId) override;
            void DrawObjects(const std::vector<Common::RenderObject> &objects) override;

            // Debug
            void SetWireframe(bool enabled) override;
            const char *GetRendererName() const override;

            // Helper method to get material's shader ID
            uint32_t GetMaterialShader(uint32_t materialId) const;

        private:
            // OpenGL objects
            GLFWwindow *m_window;
            uint32_t m_width;
            uint32_t m_height;

            GLint m_modelLoc;
            GLint m_viewLoc;
            GLint m_projectionLoc;
            GLint m_textureLoc;

            // Multi-shader management
            std::unordered_map<uint32_t, GLuint> m_shaderPrograms;
            std::unordered_map<uint32_t, ShaderUniforms> m_shaderUniforms;
            uint32_t m_nextShaderId;
            uint32_t m_currentShader;

            // View/Projection matrices
            float m_viewMatrix[16];
            float m_projectionMatrix[16];

            // Resource containers
            std::unordered_map<uint32_t, Mesh> m_meshes;
            std::unordered_map<uint32_t, Texture> m_textures;
            std::unordered_map<uint32_t, Material> m_materials;

            // Resource ID counters
            uint32_t m_nextMeshId;
            uint32_t m_nextTextureId;
            uint32_t m_nextMaterialId;

            // State
            bool m_wireframeEnabled;

            float m_clearColor[4];

            std::string LoadShaderFromFile(const std::string &filepath);
            GLuint CompileShader(const char *source, GLenum shaderType);
            GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader);
            bool CheckCompileErrors(GLuint shader, const std::string &type);
            void SetMatrix4fv(GLint location, const float *matrix);
            GLenum GetOpenGLFormat(uint32_t channels);
            GLenum GetOpenGLInternalFormat(uint32_t channels);
        };

        // Factory function implementation
        std::unique_ptr<Common::IRenderer> CreateOpenGLRenderer();
    }
}