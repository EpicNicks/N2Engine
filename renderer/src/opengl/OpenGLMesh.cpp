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
    m_vertexCapacity = meshData.vertices.size();
    m_indexCapacity = meshData.indices.size();

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

bool OpenGLMesh::Update(const Common::MeshData &meshData)
{
    if (!m_isValid || meshData.vertices.empty())
        return false;

    const auto vertexBytes = static_cast<GLsizeiptr>(meshData.vertices.size() * sizeof(Common::Vertex));
    const auto indexBytes = static_cast<GLsizeiptr>(meshData.indices.size() * sizeof(uint32_t));

    // The element buffer binding is part of the VAO, so bind the VAO before touching it. The attribute
    // pointers refer to the buffer objects, not their storage, so reallocating a buffer keeps them.
    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    if (meshData.vertices.size() <= m_vertexCapacity)
    {
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertexBytes, meshData.vertices.data());
    }
    else
    {
        // A mesh that is updated once is likely to be updated again
        glBufferData(GL_ARRAY_BUFFER, vertexBytes, meshData.vertices.data(), GL_DYNAMIC_DRAW);
        m_vertexCapacity = meshData.vertices.size();
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    if (meshData.indices.size() <= m_indexCapacity)
    {
        if (indexBytes > 0)
        {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexBytes, meshData.indices.data());
        }
    }
    else
    {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexBytes, meshData.indices.data(), GL_DYNAMIC_DRAW);
        m_indexCapacity = meshData.indices.size();
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_vertexCount = static_cast<uint32_t>(meshData.vertices.size());
    m_indexCount = static_cast<uint32_t>(meshData.indices.size());
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
        m_vertexCapacity = m_indexCapacity = 0;
        m_isValid = false;
    }
}
