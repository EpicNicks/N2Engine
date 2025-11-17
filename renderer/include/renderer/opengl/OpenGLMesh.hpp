// renderer/opengl/OpenGLMesh.hpp
#pragma once

#include <glad/glad.h>

#include "renderer/common/IMesh.hpp"
#include "renderer/common/RenderTypes.hpp"

namespace Renderer::OpenGL
{
    class OpenGLMesh : public Common::IMesh
    {
    public:
        OpenGLMesh() = default;
        ~OpenGLMesh() override;

        bool Initialize(const Common::MeshData &meshData);
        void Destroy();

        bool IsValid() const override { return m_isValid; }
        uint32_t GetIndexCount() const override { return m_indexCount; }
        uint32_t GetVertexCount() const override { return m_vertexCount; }

        // OpenGL-specific accessors
        GLuint GetVAO() const { return m_VAO; }
        GLuint GetVBO() const { return m_VBO; }
        GLuint GetEBO() const { return m_EBO; }

    private:
        GLuint m_VAO = 0;
        GLuint m_VBO = 0;
        GLuint m_EBO = 0;
        uint32_t m_indexCount = 0;
        uint32_t m_vertexCount = 0;
        bool m_isValid = false;
    };
}