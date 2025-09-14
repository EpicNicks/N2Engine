#include "renderer/opengl/OpenGLRenderer.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

using namespace Renderer::OpenGL;

OpenGLRenderer::OpenGLRenderer()
    : m_window(nullptr), m_width(0), m_height(0), m_shaderProgram(0), m_modelLoc(-1), m_viewLoc(-1), m_projectionLoc(-1), m_textureLoc(-1), m_nextMeshId(1), m_nextTextureId(1), m_nextMaterialId(1), m_wireframeEnabled(false)
{
    // Initialize matrices to identity
    memset(m_viewMatrix, 0, sizeof(m_viewMatrix));
    memset(m_projectionMatrix, 0, sizeof(m_projectionMatrix));

    // Set diagonal to 1 for identity matrices
    for (int i = 0; i < 4; ++i)
    {
        m_viewMatrix[i * 4 + i] = 1.0f;
        m_projectionMatrix[i * 4 + i] = 1.0f;
    }
}

OpenGLRenderer::~OpenGLRenderer()
{
    Shutdown();
}

void OpenGLRenderer::Clear(float r, float g, float b, float a)
{
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

bool OpenGLRenderer::Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height)
{
    m_window = windowHandle;
    m_width = width;
    m_height = height;

    // Make context current
    glfwMakeContextCurrent(m_window);

    if (!gladLoadGL())
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return false;
    }

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    std::cout << "OpenGL Vendor: " << glGetString(GL_VENDOR) << std::endl;
    std::cout << "OpenGL Renderer: " << glGetString(GL_RENDERER) << std::endl;

    // Set viewport
    glViewport(0, 0, width, height);

    // Enable depth testing
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Note: Shader program creation is now handled externally
    // Use CreateShaderProgram(vertexSource, fragmentSource) or LoadShaderProgram(vertexPath, fragmentPath) after initialization

    return true;
}

void OpenGLRenderer::Shutdown()
{
    // Clean up meshes
    for (auto &pair : m_meshes)
    {
        if (pair.second.isValid)
        {
            glDeleteVertexArrays(1, &pair.second.VAO);
            glDeleteBuffers(1, &pair.second.VBO);
            glDeleteBuffers(1, &pair.second.EBO);
        }
    }
    m_meshes.clear();

    // Clean up textures
    for (auto &pair : m_textures)
    {
        if (pair.second.isValid)
        {
            glDeleteTextures(1, &pair.second.handle);
        }
    }
    m_textures.clear();

    // Clear materials
    m_materials.clear();

    // Clean up shader program
    if (m_shaderProgram)
    {
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
    }
}

void OpenGLRenderer::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
}

bool OpenGLRenderer::CreateShaderProgram(const char *vertexSource, const char *fragmentSource)
{
    if (!vertexSource || !fragmentSource)
    {
        std::cerr << "Invalid shader source provided" << std::endl;
        return false;
    }

    GLuint vertexShader = CompileShader(vertexSource, GL_VERTEX_SHADER);
    if (vertexShader == 0)
    {
        return false;
    }

    GLuint fragmentShader = CompileShader(fragmentSource, GL_FRAGMENT_SHADER);
    if (fragmentShader == 0)
    {
        glDeleteShader(vertexShader);
        return false;
    }

    // Clean up existing program if it exists
    if (m_shaderProgram)
    {
        glDeleteProgram(m_shaderProgram);
    }

    m_shaderProgram = LinkProgram(vertexShader, fragmentShader);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (m_shaderProgram == 0)
    {
        return false;
    }

    // Get uniform locations
    m_modelLoc = glGetUniformLocation(m_shaderProgram, "uModel");
    m_viewLoc = glGetUniformLocation(m_shaderProgram, "uView");
    m_projectionLoc = glGetUniformLocation(m_shaderProgram, "uProjection");
    m_textureLoc = glGetUniformLocation(m_shaderProgram, "uTexture");

    return true;
}

bool OpenGLRenderer::LoadShaderProgram(const std::string &vertexPath, const std::string &fragmentPath)
{
    std::string vertexSource = LoadShaderFromFile(vertexPath);
    std::string fragmentSource = LoadShaderFromFile(fragmentPath);

    if (vertexSource.empty() || fragmentSource.empty())
    {
        return false;
    }

    return CreateShaderProgram(vertexSource.c_str(), fragmentSource.c_str());
}

std::string OpenGLRenderer::LoadShaderFromFile(const std::string &filepath)
{
    std::ifstream file;
    std::stringstream stream;

    // Ensure ifstream can throw exceptions
    file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try
    {
        file.open(filepath);
        stream << file.rdbuf();
        file.close();
        return stream.str();
    }
    catch (std::ifstream::failure &e)
    {
        std::cerr << "Failed to read shader file: " << filepath << " - " << e.what() << std::endl;
        return "";
    }
}

void OpenGLRenderer::BeginFrame()
{
    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Use our shader program (if one is loaded)
    if (m_shaderProgram)
    {
        glUseProgram(m_shaderProgram);

        // Set view and projection matrices
        SetMatrix4fv(m_viewLoc, m_viewMatrix);
        SetMatrix4fv(m_projectionLoc, m_projectionMatrix);
    }

    // Set wireframe mode
    if (m_wireframeEnabled)
    {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    else
    {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

void OpenGLRenderer::EndFrame()
{
    // Nothing specific needed for OpenGL
}

void OpenGLRenderer::Present()
{
    glfwSwapBuffers(m_window);
}

uint32_t OpenGLRenderer::CreateMesh(const Common::MeshData &meshData)
{
    if (meshData.vertices.empty())
    {
        return 0;
    }

    Mesh mesh = {};
    mesh.indexCount = static_cast<uint32_t>(meshData.indices.size());

    // Generate OpenGL objects
    glGenVertexArrays(1, &mesh.VAO);
    glGenBuffers(1, &mesh.VBO);
    glGenBuffers(1, &mesh.EBO);

    // Bind VAO first
    glBindVertexArray(mesh.VAO);

    // Upload vertex data
    glBindBuffer(GL_ARRAY_BUFFER, mesh.VBO);
    glBufferData(GL_ARRAY_BUFFER,
                 meshData.vertices.size() * sizeof(Common::Vertex),
                 meshData.vertices.data(),
                 GL_STATIC_DRAW);

    // Upload index data
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 meshData.indices.size() * sizeof(uint32_t),
                 meshData.indices.data(),
                 GL_STATIC_DRAW);

    // Set vertex attributes
    // Position (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex), (void *)offsetof(Common::Vertex, position));
    glEnableVertexAttribArray(0);

    // Normal (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex), (void *)offsetof(Common::Vertex, normal));
    glEnableVertexAttribArray(1);

    // Texture coordinates (location 2)
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex), (void *)offsetof(Common::Vertex, texCoord));
    glEnableVertexAttribArray(2);

    // Color (location 3)
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex), (void *)offsetof(Common::Vertex, color));
    glEnableVertexAttribArray(3);

    // Unbind VAO
    glBindVertexArray(0);

    mesh.isValid = true;

    uint32_t meshId = m_nextMeshId++;
    m_meshes[meshId] = mesh;
    return meshId;
}

void OpenGLRenderer::DestroyMesh(uint32_t meshId)
{
    auto it = m_meshes.find(meshId);
    if (it != m_meshes.end() && it->second.isValid)
    {
        glDeleteVertexArrays(1, &it->second.VAO);
        glDeleteBuffers(1, &it->second.VBO);
        glDeleteBuffers(1, &it->second.EBO);
        m_meshes.erase(it);
    }
}

uint32_t OpenGLRenderer::CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels)
{
    if (!data || width == 0 || height == 0 || channels == 0)
    {
        return 0;
    }

    Texture texture = {};
    texture.width = width;
    texture.height = height;
    texture.channels = channels;

    glGenTextures(1, &texture.handle);
    glBindTexture(GL_TEXTURE_2D, texture.handle);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Upload texture data
    GLenum format = GetOpenGLFormat(channels);
    GLenum internalFormat = GetOpenGLInternalFormat(channels);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);

    texture.isValid = true;

    uint32_t textureId = m_nextTextureId++;
    m_textures[textureId] = texture;
    return textureId;
}

void OpenGLRenderer::DestroyTexture(uint32_t textureId)
{
    auto it = m_textures.find(textureId);
    if (it != m_textures.end() && it->second.isValid)
    {
        glDeleteTextures(1, &it->second.handle);
        m_textures.erase(it);
    }
}

uint32_t OpenGLRenderer::CreateMaterial(uint32_t textureId)
{
    auto texIt = m_textures.find(textureId);
    if (texIt == m_textures.end() || !texIt->second.isValid)
    {
        return 0;
    }

    Material material = {};
    material.textureId = textureId;
    material.isValid = true;

    uint32_t materialId = m_nextMaterialId++;
    m_materials[materialId] = material;
    return materialId;
}

void OpenGLRenderer::DestroyMaterial(uint32_t materialId)
{
    m_materials.erase(materialId);
}

void OpenGLRenderer::SetViewProjection(const float *view, const float *projection)
{
    if (view)
    {
        memcpy(m_viewMatrix, view, sizeof(m_viewMatrix));
    }
    if (projection)
    {
        memcpy(m_projectionMatrix, projection, sizeof(m_projectionMatrix));
    }
}

void OpenGLRenderer::DrawMesh(uint32_t meshId, const float *modelMatrix, uint32_t materialId)
{
    if (!m_shaderProgram)
    {
        std::cerr << "No shader program loaded! Call CreateShaderProgram() or LoadShaderProgram() first." << std::endl;
        return;
    }

    auto meshIt = m_meshes.find(meshId);
    if (meshIt == m_meshes.end() || !meshIt->second.isValid)
    {
        return;
    }

    auto materialIt = m_materials.find(materialId);
    if (materialIt == m_materials.end() || !materialIt->second.isValid)
    {
        return;
    }

    auto textureIt = m_textures.find(materialIt->second.textureId);
    if (textureIt == m_textures.end() || !textureIt->second.isValid)
    {
        return;
    }

    // Set model matrix
    SetMatrix4fv(m_modelLoc, modelMatrix);

    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureIt->second.handle);
    glUniform1i(m_textureLoc, 0);

    // Draw mesh
    const Mesh &mesh = meshIt->second;
    glBindVertexArray(mesh.VAO);
    glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void OpenGLRenderer::DrawObjects(const std::vector<Common::RenderObject> &objects)
{
    for (const auto &obj : objects)
    {
        DrawMesh(obj.meshId, obj.transform.model, obj.materialId);
    }
}

void OpenGLRenderer::SetWireframe(bool enabled)
{
    m_wireframeEnabled = enabled;
}

const char *OpenGLRenderer::GetRendererName() const
{
    return "OpenGL Renderer";
}

GLuint OpenGLRenderer::CompileShader(const char *source, GLenum shaderType)
{
    GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    if (!CheckCompileErrors(shader, shaderType == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT"))
    {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint OpenGLRenderer::LinkProgram(GLuint vertexShader, GLuint fragmentShader)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    if (!CheckCompileErrors(program, "PROGRAM"))
    {
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

bool OpenGLRenderer::CheckCompileErrors(GLuint shader, const std::string &type)
{
    GLint success;
    GLchar infoLog[1024];

    if (type != "PROGRAM")
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            std::cerr << "Shader compilation error (" << type << "): " << infoLog << std::endl;
            return false;
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            std::cerr << "Program linking error: " << infoLog << std::endl;
            return false;
        }
    }

    return true;
}

void OpenGLRenderer::SetMatrix4fv(GLint location, const float *matrix)
{
    if (location != -1)
    {
        glUniformMatrix4fv(location, 1, GL_FALSE, matrix);
    }
}

GLenum OpenGLRenderer::GetOpenGLFormat(uint32_t channels)
{
    switch (channels)
    {
    case 1:
        return GL_RED;
    case 2:
        return GL_RG;
    case 3:
        return GL_RGB;
    case 4:
        return GL_RGBA;
    default:
        return GL_RGB;
    }
}

GLenum OpenGLRenderer::GetOpenGLInternalFormat(uint32_t channels)
{
    switch (channels)
    {
    case 1:
        return GL_R8;
    case 2:
        return GL_RG8;
    case 3:
        return GL_RGB8;
    case 4:
        return GL_RGBA8;
    default:
        return GL_RGB8;
    }
}

// Factory function
std::unique_ptr<Renderer::Common::IRenderer> CreateOpenGLRenderer()
{
    return std::make_unique<OpenGLRenderer>();
}
