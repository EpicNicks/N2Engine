#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

#include <math/Matrix.hpp>

#include "renderer/opengl/OpenGLRenderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"

using namespace Renderer::OpenGL;

OpenGLRenderer::OpenGLRenderer()
    : m_window(nullptr), m_width(0), m_height(0),
      m_currentShader(0), m_nextMeshId(1), m_nextTextureId(1), m_wireframeEnabled(false)
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
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
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

    CreateStandardShaders();

    return true;
}

void OpenGLRenderer::Shutdown()
{
    // Clean up materials first
    m_materials.clear();

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

    // Clean up shader programs
    m_shaderPrograms.clear(); // shared_ptr handles cleanup

    m_currentShader = 0;
}

void OpenGLRenderer::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
}

Renderer::Common::IShader *OpenGLRenderer::CreateShaderProgram(const char *vertexSource, const char *fragmentSource)
{
    auto shader = std::make_shared<OpenGLShader>(); // shared_ptr instead of unique_ptr

    if (!shader->LoadFromStrings(vertexSource, fragmentSource))
    {
        std::cerr << "Failed to create shader program" << std::endl;
        return 0;
    }

    m_shaderPrograms[shader->GetId()] = shader;
    return shader.get();
}

void OpenGLRenderer::UseShaderProgram(Common::IShader *shader)
{
    if (!shader)
    {
        return;
    }
    if (auto *openglShader = dynamic_cast<OpenGLShader *>(shader))
    {
        GLuint shaderId = openglShader->GetId();
        if (m_currentShader != shaderId)
        {
            auto it = m_shaderPrograms.find(shaderId);
            if (it != m_shaderPrograms.end())
            {
                it->second->Bind();
                m_currentShader = shaderId;
            }
        }
    }
    else
    {
        std::cerr << "Error: Attempted to use a non-OpenGL shader with OpenGLRenderer." << std::endl;
    }
}

bool OpenGLRenderer::DestroyShaderProgram(Common::IShader *shader)
{
    if (!shader)
    {
        return false;
    }
    if (auto *openglShader = dynamic_cast<OpenGLShader *>(shader))
    {
        GLuint shaderId = openglShader->GetId();
        auto it = m_shaderPrograms.find(shaderId);
        if (it != m_shaderPrograms.end())
        {
            m_shaderPrograms.erase(it); // unique_ptr handles cleanup
            if (m_currentShader == shaderId)
            {
                m_currentShader = 0;
            }
            return true;
        }
        return false;
    }
    else
    {
        std::cerr << "Error: Attempted to destroy a non-OpenGL shader with OpenGLRenderer." << std::endl;
        return false;
    }
}

bool OpenGLRenderer::IsValidShader(uint32_t shaderId) const
{
    auto it = m_shaderPrograms.find(shaderId);
    return it != m_shaderPrograms.end() && it->second->IsValid();
}

void OpenGLRenderer::BeginFrame()
{
    glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // temporary
    // glDisable(GL_DEPTH_TEST);
    // glDisable(GL_CULL_FACE);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

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

Renderer::Common::IMaterial *OpenGLRenderer::CreateMaterial(Common::IShader *shader, uint32_t textureId)
{
    if (!shader)
    {
        std::cerr << "Null shader provided to CreateMaterial." << std::endl;
        return nullptr;
    }

    auto *openglShader = dynamic_cast<OpenGLShader *>(shader);
    if (!openglShader)
    {
        std::cerr << "Error: Attempted to create material with a non-OpenGL shader." << std::endl;
        return nullptr;
    }

    uint32_t shaderId = openglShader->GetId();

    // Ensure shader is managed by this renderer
    auto shaderIt = m_shaderPrograms.find(shaderId);
    if (shaderIt == m_shaderPrograms.end())
    {
        std::cerr << "Shader not managed by this renderer in CreateMaterial: " << shaderId << std::endl;
        return nullptr;
    }

    // Create material with shared shader reference
    auto material = std::make_unique<OpenGLMaterial>(shaderIt->second, shaderId, textureId);
    Common::IMaterial *ptr = material.get();
    m_materials.push_back(std::move(material));

    return ptr;
}

void OpenGLRenderer::DestroyMaterial(Common::IMaterial *material)
{
    if (!material)
        return;

    // Find and remove the material
    auto it = std::find_if(m_materials.begin(), m_materials.end(),
                           [material](const std::unique_ptr<OpenGLMaterial> &mat)
                           {
                               return mat.get() == material;
                           });

    if (it != m_materials.end())
    {
        m_materials.erase(it);
    }
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

void OpenGLRenderer::DrawMesh(uint32_t meshId, const float *modelMatrix, Common::IMaterial *material)
{
    if (!material)
        return;

    auto meshIt = m_meshes.find(meshId);
    if (meshIt == m_meshes.end() || !meshIt->second.isValid)
        return;

    // Cast to OpenGL-specific material (safe - we created it)
    auto *glMaterial = static_cast<OpenGLMaterial *>(material);

    // Apply material (binds shader and sets all material properties)
    glMaterial->Apply();

    // Get shader for setting standard uniforms
    OpenGLShader *shader = glMaterial->GetShader();
    if (!shader)
        return;

    const ShaderUniforms &uniforms = shader->GetCommonUniforms();

    // Set standard transform uniforms
    if (uniforms.modelLoc != -1)
    {
        glUniformMatrix4fv(uniforms.modelLoc, 1, GL_TRUE, modelMatrix);
    }

    if (uniforms.viewLoc != -1)
    {
        glUniformMatrix4fv(uniforms.viewLoc, 1, GL_TRUE, m_viewMatrix);
    }

    if (uniforms.projectionLoc != -1)
    {
        glUniformMatrix4fv(uniforms.projectionLoc, 1, GL_TRUE, m_projectionMatrix);
    }

    // Bind texture if material has one
    uint32_t textureId = glMaterial->GetTextureId();
    if (textureId != 0)
    {
        auto textureIt = m_textures.find(textureId);
        if (textureIt != m_textures.end() && textureIt->second.isValid)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureIt->second.handle);

            if (uniforms.textureLoc != -1)
            {
                glUniform1i(uniforms.textureLoc, 0);
            }
        }
    }

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
        DrawMesh(obj.meshId, obj.transform.model, obj.material);
    }
}

void Renderer::OpenGL::OpenGLRenderer::OnResize(int width, int height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    glViewport(0, 0, width, height);

    m_width = static_cast<uint32_t>(width);
    m_height = static_cast<uint32_t>(height);
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
        // GL_TRUE to pass Row-Major (Transposed compared to default) matrices
        glUniformMatrix4fv(location, 1, GL_TRUE, matrix);
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

void OpenGLRenderer::CreateStandardShaders()
{
    // UNLIT SHADER - No lighting, just colors
    const char *unlitVert = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aNormal;
        layout (location = 2) in vec2 aTexCoord;
        layout (location = 3) in vec4 aColor;
        
        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;
        
        out vec2 fragTexCoord;
        
        void main() {
            gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
            fragTexCoord = aTexCoord;
        }
    )";

    const char *unlitFrag = R"(
        #version 330 core
        
        uniform vec4 uAlbedo;  // Material color
        uniform sampler2D uTexture;
        uniform bool uHasTexture;
        
        in vec2 fragTexCoord;
        out vec4 FragColor;
        
        void main() {
            vec4 color = uAlbedo;
            if (uHasTexture) {
                color *= texture(uTexture, fragTexCoord);
            }
            FragColor = color;
        }
    )";

    m_standardUnlitShader = CreateShaderProgram(unlitVert, unlitFrag);

    // STANDARD LIT SHADER - Simple directional + ambient
    const char *litVert = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aNormal;
        layout (location = 2) in vec2 aTexCoord;
        layout (location = 3) in vec4 aColor;
        
        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;
        
        out vec3 fragNormal;
        out vec3 fragWorldPos;
        out vec2 fragTexCoord;
        
        void main() {
            vec4 worldPos = uModel * vec4(aPos, 1.0);
            fragWorldPos = worldPos.xyz;
            fragNormal = mat3(uModel) * aNormal;  // Transform normal to world space
            fragTexCoord = aTexCoord;
            gl_Position = uProjection * uView * worldPos;
        }
    )";

    const char *litFrag = R"(
        #version 330 core
        
        // Material properties
        uniform vec4 uAlbedo;
        uniform sampler2D uTexture;
        uniform bool uHasTexture;
        
        // Scene lighting (set by renderer each frame)
        uniform vec3 uAmbientLight;
        uniform vec3 uDirectionalLightDir;
        uniform vec3 uDirectionalLightColor;
        uniform float uDirectionalLightIntensity;
        
        in vec3 fragNormal;
        in vec3 fragWorldPos;
        in vec2 fragTexCoord;
        
        out vec4 FragColor;
        
        void main() {
            // Get base color
            vec4 albedo = uAlbedo;
            if (uHasTexture) {
                albedo *= texture(uTexture, fragTexCoord);
            }
            
            // Ambient
            vec3 ambient = uAmbientLight;
            
            // Directional light
            vec3 N = normalize(fragNormal);
            vec3 L = normalize(-uDirectionalLightDir);
            float NdotL = max(dot(N, L), 0.0);
            vec3 diffuse = uDirectionalLightColor * uDirectionalLightIntensity * NdotL;
            
            // Combine
            vec3 lighting = ambient + diffuse;
            FragColor = vec4(lighting * albedo.rgb, albedo.a);
        }
    )";

    m_standardLitShader = CreateShaderProgram(litVert, litFrag);
}

Renderer::Common::IShader *OpenGLRenderer::GetStandardUnlitShader() const
{
    return m_standardUnlitShader;
}

Renderer::Common::IShader *OpenGLRenderer::GetStandardLitShader() const
{
    return m_standardLitShader;
}

// Factory function
std::unique_ptr<Renderer::Common::IRenderer> Renderer::OpenGL::CreateOpenGLRenderer()
{
    return std::make_unique<OpenGLRenderer>();
}
