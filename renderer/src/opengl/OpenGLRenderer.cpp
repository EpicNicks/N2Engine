#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

#include <math/Matrix.hpp>

#include "renderer/opengl/OpenGLRenderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLMesh.hpp"

using namespace Renderer::OpenGL;

OpenGLRenderer::OpenGLRenderer()
    : m_window(nullptr), m_width(0), m_height(0),
      m_currentShader(0), m_wireframeEnabled(false)
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
    m_materials.clear(); // Destroy materials first
    m_meshes.clear();    // Then meshes
    m_textures.clear();
    m_shaderPrograms.clear();
}

void OpenGLRenderer::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
}

Renderer::Common::IShader *OpenGLRenderer::CreateShaderProgram(const char *vertexSource, const char *fragmentSource)
{
    auto shader = std::make_shared<OpenGLShader>();

    if (!shader->LoadFromStrings(vertexSource, fragmentSource))
    {
        std::cerr << "Failed to create shader program" << std::endl;
        return nullptr;
    }

    Common::IShader *ptr = shader.get();
    m_shaderPrograms[ptr] = shader;
    return ptr;
}

void OpenGLRenderer::UseShaderProgram(Common::IShader *shader)
{
    if (!shader)
    {
        return;
    }

    auto *openglShader = dynamic_cast<OpenGLShader *>(shader);
    if (!openglShader)
    {
        std::cerr << "Error: Non-OpenGL shader with OpenGLRenderer." << std::endl;
        return;
    }

    GLuint shaderId = openglShader->GetId();
    if (m_currentShader != shaderId)
    {
        openglShader->Bind();
        m_currentShader = shaderId;
    }
}

bool OpenGLRenderer::DestroyShaderProgram(Common::IShader *shader)
{
    if (!shader)
    {
        return false;
    }

    if (auto it = m_shaderPrograms.find(shader); it != m_shaderPrograms.end())
    {
        if (it->second->GetId() == m_currentShader)
        {
            m_currentShader = 0;
        }
        m_shaderPrograms.erase(it);
        return true;
    }
    return false;
}

bool OpenGLRenderer::IsValidShader(Common::IShader *shader) const
{
    if (!shader)
    {
        return false;
    }

    auto it = m_shaderPrograms.find(shader);
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

Renderer::Common::IMesh *OpenGLRenderer::CreateMesh(const Common::MeshData &meshData)
{
    auto mesh = std::make_unique<OpenGLMesh>();

    if (!mesh->Initialize(meshData))
    {
        std::cerr << "Failed to create mesh" << std::endl;
        return nullptr;
    }

    Common::IMesh *ptr = mesh.get();
    m_meshes[ptr] = std::move(mesh);
    return ptr;
}

void OpenGLRenderer::DestroyMesh(Common::IMesh *mesh)
{
    if (!mesh)
        return;

    m_meshes.erase(mesh);
}

Renderer::Common::ITexture *OpenGLRenderer::CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels)
{
    auto texture = std::make_unique<OpenGLTexture>();

    if (!texture->Initialize(data, width, height, channels))
    {
        std::cerr << "Failed to create texture" << std::endl;
        return nullptr;
    }

    Common::ITexture *ptr = texture.get();
    m_textures[ptr] = std::move(texture);
    return ptr;
}

void OpenGLRenderer::DestroyTexture(Renderer::Common::ITexture *texture)
{
    if (!texture)
    {
        return;
    }
    m_textures.erase(texture);
}

Renderer::Common::IMaterial *OpenGLRenderer::CreateMaterial(Renderer::Common::IShader *shader, Renderer::Common::ITexture *texture)
{
    if (!shader)
    {
        std::cerr << "Null shader provided to CreateMaterial." << std::endl;
        return nullptr;
    }

    auto *openglShader = dynamic_cast<OpenGLShader *>(shader);
    if (!openglShader)
    {
        std::cerr << "Error: Non-OpenGL shader in CreateMaterial." << std::endl;
        return nullptr;
    }

    // Ensure shader is managed by this renderer
    auto shaderIt = m_shaderPrograms.find(shader);
    if (shaderIt == m_shaderPrograms.end())
    {
        std::cerr << "Shader not managed by this renderer." << std::endl;
        return nullptr;
    }

    // Create material with shared shader reference
    auto material = std::make_unique<OpenGLMaterial>(shaderIt->second, texture);
    Common::IMaterial *ptr = material.get();
    m_materials[ptr] = std::move(material);

    return ptr;
}

void OpenGLRenderer::DestroyMaterial(Renderer::Common::IMaterial *material)
{
    if (!material)
    {
        return;
    }

    m_materials.erase(material);
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

void OpenGLRenderer::DrawMesh(Renderer::Common::IMesh *mesh, const float *modelMatrix, Renderer::Common::IMaterial *material)
{
    if (!mesh || !mesh->IsValid() || !material)
    {
        return;
    }

    // implicitly safe cast to OpenGL-specific types created
    auto *glMesh = static_cast<OpenGLMesh *>(mesh);
    auto *glMaterial = static_cast<OpenGLMaterial *>(material);

    // Apply material (binds shader and sets material properties)
    glMaterial->Apply();

    // Get shader for setting standard uniforms
    OpenGLShader *shader = glMaterial->GetShader();
    if (!shader)
    {
        return;
    }

    const ShaderUniforms &uniforms = shader->GetCommonUniforms();

    // Set transform uniforms
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
    OpenGLTexture *texture = glMaterial->GetTexture();
    if (texture && texture->IsValid())
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture->GetHandle());

        if (uniforms.textureLoc != -1)
        {
            glUniform1i(uniforms.textureLoc, 0);
        }
    }

    // Draw mesh
    glBindVertexArray(glMesh->GetVAO());
    glDrawElements(GL_TRIANGLES, glMesh->GetIndexCount(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void OpenGLRenderer::DrawObjects(const std::vector<Common::RenderObject> &objects)
{
    for (const auto &obj : objects)
    {
        DrawMesh(obj.mesh, obj.transform.model, obj.material);
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
