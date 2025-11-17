#include "renderer/opengl/OpenGLMesh.hpp"

using namespace Renderer::OpenGL;

OpenGLMesh::~OpenGLMesh()
{
    Destroy();
}

bool OpenGLMesh::Initialize(const Common::MeshData &meshData)
{
    if (meshData.vertices.empty())
        return false;

    m_indexCount = static_cast<uint32_t>(meshData.indices.size());
    m_vertexCount = static_cast<uint32_t>(meshData.vertices.size());

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);

    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER,
                 meshData.vertices.size() * sizeof(Common::Vertex),
                 meshData.vertices.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 meshData.indices.size() * sizeof(uint32_t),
                 meshData.indices.data(),
                 GL_STATIC_DRAW);

    // Position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex),
                          (void *)offsetof(Common::Vertex, position));
    glEnableVertexAttribArray(0);

    // Normal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex),
                          (void *)offsetof(Common::Vertex, normal));
    glEnableVertexAttribArray(1);

    // TexCoord
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex),
                          (void *)offsetof(Common::Vertex, texCoord));
    glEnableVertexAttribArray(2);

    // Color
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex),
                          (void *)offsetof(Common::Vertex, color));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    m_isValid = true;
    return true;
}

void OpenGLMesh::Destroy()
{
    if (m_isValid)
    {
        glDeleteVertexArrays(1, &m_VAO);
        glDeleteBuffers(1, &m_VBO);
        glDeleteBuffers(1, &m_EBO);
        m_VAO = m_VBO = m_EBO = 0;
        m_isValid = false;
    }
}