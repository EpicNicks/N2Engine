#include <iostream>
#include <algorithm>

#include <math/Matrix.hpp>

#include "renderer/opengl/OpenGLRenderer.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLMesh.hpp"

using namespace Renderer::OpenGL;

OpenGLRenderer::OpenGLRenderer()
    : m_window(nullptr), m_width(0), m_height(0), m_standardUnlitShader(nullptr), m_standardLitShader(nullptr),
      m_viewMatrix{}, m_projectionMatrix{}, m_currentShader(0), m_wireframeEnabled(false), m_clearColor{}
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
    OpenGLRenderer::Shutdown();
}

void OpenGLRenderer::Clear(const float r, const float g, const float b, const float a)
{
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

bool OpenGLRenderer::Initialize(GLFWwindow *windowHandle, const uint32_t width, const uint32_t height)
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
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

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
    m_meshes.clear(); // Then meshes
    m_textures.clear();
    m_shaderPrograms.clear();
}

void OpenGLRenderer::Resize(const uint32_t width, const uint32_t height)
{
    m_width = width;
    m_height = height;
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

Renderer::Common::IShader* OpenGLRenderer::CreateShaderProgram(const char *vertexSource, const char *fragmentSource)
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

    auto *openglShader = dynamic_cast<OpenGLShader*>(shader);
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

Renderer::Common::IMesh* OpenGLRenderer::CreateMesh(const Common::MeshData &meshData)
{
    auto mesh = std::make_unique<OpenGLMesh>();

    if (!mesh->Initialize(meshData))
    {
        std::cerr << "Failed to create mesh" << std::endl;
        return nullptr;
    }

    auto [iter, inserted] = m_meshes.try_emplace(mesh.get(), std::move(mesh));
    return iter->first;
}

void OpenGLRenderer::DestroyMesh(Common::IMesh *mesh)
{
    if (!mesh)
        return;

    m_meshes.erase(mesh);
}

Renderer::Common::ITexture* OpenGLRenderer::CreateTexture(const uint8_t *data, const uint32_t width,
                                                          const uint32_t height,
                                                          const uint32_t channels)
{
    auto texture = std::make_unique<OpenGLTexture>();

    if (!texture->Initialize(data, width, height, channels))
    {
        std::cerr << "Failed to create texture" << std::endl;
        return nullptr;
    }

    auto [iter, inserted] = m_textures.try_emplace(texture.get(), std::move(texture));
    return iter->first;
}

void OpenGLRenderer::DestroyTexture(Common::ITexture *texture)
{
    if (!texture)
    {
        return;
    }
    m_textures.erase(texture);
}

Renderer::Common::IMaterial* OpenGLRenderer::CreateMaterial(Common::IShader* shader)
{
    return OpenGLRenderer::CreateMaterial(shader, nullptr);
}

Renderer::Common::IMaterial* OpenGLRenderer::CreateMaterial(Common::IShader *shader, Common::ITexture *texture)
{
    if (!shader)
    {
        std::cerr << "Null shader provided to CreateMaterial." << std::endl;
        return nullptr;
    }

    if (const auto *openglShader = dynamic_cast<OpenGLShader*>(shader); !openglShader)
    {
        std::cerr << "Error: Non-OpenGL shader in CreateMaterial." << std::endl;
        return nullptr;
    }

    // Ensure shader is managed by this renderer
    const auto shaderIt = m_shaderPrograms.find(shader);
    if (shaderIt == m_shaderPrograms.end())
    {
        std::cerr << "Shader not managed by this renderer." << std::endl;
        return nullptr;
    }

    auto material = std::make_unique<OpenGLMaterial>(shaderIt->second, texture);

    if (shader == m_standardLitShader)
    {
        // lit shader defaults
        material->SetFloat("uMetallic", 0.0f);
        material->SetFloat("uSmoothness", 0.5f);
        material->SetInt("uHasTexture", texture != nullptr ? 1 : 0);
    }
    else if (shader == m_standardUnlitShader)
    {
        material->SetInt("uHasTexture", texture != nullptr ? 1 : 0);
    }
    // ⭐ END ADD ⭐

    auto [iter, inserted] = m_materials.try_emplace(material.get(), std::move(material));
    return iter->first;
}

void OpenGLRenderer::DestroyMaterial(Common::IMaterial *material)
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

void OpenGLRenderer::UpdateSceneLighting(
    const Common::SceneLightingData &lighting,
    const N2Engine::Math::Vector3 &cameraPosition)
{
    m_currentLighting = lighting;

    // Update lit shader with lighting data
    if (!m_standardLitShader)
        return;

    auto shaderIt = m_shaderPrograms.find(m_standardLitShader);
    if (shaderIt == m_shaderPrograms.end())
        return;

    auto *shader = static_cast<OpenGLShader*>(shaderIt->second.get());
    shader->Bind();

    // Set ambient
    shader->SetVec3("uAmbientLight",
                    lighting.ambientColor.x,
                    lighting.ambientColor.y,
                    lighting.ambientColor.z);

    // Set camera position directly from parameter
    shader->SetVec3("uCameraPos",
                    cameraPosition.x,
                    cameraPosition.y,
                    cameraPosition.z);

    // Set directional lights
    const int numDirLights = std::min<int>(static_cast<int>(lighting.directionalLights.size()),
                                           Common::SceneLightingData::MAX_DIRECTIONAL_LIGHTS);
    shader->SetInt("uNumDirectionalLights", numDirLights);

    for (int i = 0; i < numDirLights; ++i)
    {
        const auto &light = lighting.directionalLights[i];
        std::string base = "uDirectionalLights[" + std::to_string(i) + "]";

        shader->SetVec3(base + ".direction",
                        light.direction.x, light.direction.y, light.direction.z);
        shader->SetVec3(base + ".color",
                        light.color.x, light.color.y, light.color.z);
        shader->SetFloat(base + ".intensity", light.intensity);
    }

    // Set point lights
    const int numPointLights = std::min<int>(static_cast<int>(lighting.pointLights.size()),
                                             Common::SceneLightingData::MAX_POINT_LIGHTS);
    shader->SetInt("uNumPointLights", numPointLights);

    for (int i = 0; i < numPointLights; ++i)
    {
        const auto &light = lighting.pointLights[i];
        std::string base = "uPointLights[" + std::to_string(i) + "]";

        shader->SetVec3(base + ".position",
                        light.position.x, light.position.y, light.position.z);
        shader->SetVec3(base + ".color",
                        light.color.x, light.color.y, light.color.z);
        shader->SetFloat(base + ".intensity", light.intensity);
        shader->SetFloat(base + ".range", light.range);
        shader->SetFloat(base + ".attenuation", light.attenuation);
    }

    // Set spot lights
    const int numSpotLights = std::min<int>(static_cast<int>(lighting.spotLights.size()),
                                            Common::SceneLightingData::MAX_SPOT_LIGHTS);
    shader->SetInt("uNumSpotLights", numSpotLights);

    for (int i = 0; i < numSpotLights; ++i)
    {
        const auto &light = lighting.spotLights[i];
        std::string base = "uSpotLights[" + std::to_string(i) + "]";

        shader->SetVec3(base + ".position",
                        light.position.x, light.position.y, light.position.z);
        shader->SetVec3(base + ".direction",
                        light.direction.x, light.direction.y, light.direction.z);
        shader->SetVec3(base + ".color",
                        light.color.x, light.color.y, light.color.z);
        shader->SetFloat(base + ".intensity", light.intensity);
        shader->SetFloat(base + ".range", light.range);
        shader->SetFloat(base + ".innerConeAngle", light.innerConeAngle);
        shader->SetFloat(base + ".outerConeAngle", light.outerConeAngle);
    }
}


void OpenGLRenderer::DrawMesh(Common::IMesh *mesh, const float *modelMatrix, Common::IMaterial *material)
{
    if (!mesh || !mesh->IsValid() || !material)
    {
        return;
    }

    // implicitly safe cast to OpenGL-specific types created
    const auto *glMesh = dynamic_cast<OpenGLMesh*>(mesh);
    auto *glMaterial = dynamic_cast<OpenGLMaterial*>(material);

    // Apply material (binds shader and sets material properties)
    glMaterial->Apply();

    // Get shader for setting standard uniforms
    const OpenGLShader *shader = glMaterial->GetShader();
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
    if (const OpenGLTexture *texture = glMaterial->GetTexture(); texture && texture->IsValid())
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
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(glMesh->GetIndexCount()), GL_UNSIGNED_INT, nullptr);
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

const char* OpenGLRenderer::GetRendererName() const
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

void OpenGLRenderer::SetMatrix4fv(const GLint location, const float *matrix)
{
    if (location != -1)
    {
        // GL_TRUE to pass Row-Major (Transposed compared to default) matrices
        glUniformMatrix4fv(location, 1, GL_TRUE, matrix);
    }
}

GLenum OpenGLRenderer::GetOpenGLFormat(const uint32_t channels)
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

GLenum OpenGLRenderer::GetOpenGLInternalFormat(const uint32_t channels)
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
    // ================== UNLIT SHADER (Keep as-is) ==================
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

        uniform vec4 uAlbedo;
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

    // ================== LIT SHADER (REPLACE THIS ENTIRE SECTION) ==================
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

            // Transform normal to world space (proper method)
            mat3 normalMatrix = transpose(inverse(mat3(uModel)));
            fragNormal = normalize(normalMatrix * aNormal);

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
        uniform float uMetallic;
        uniform float uSmoothness;

        // Camera
        uniform vec3 uCameraPos;

        // Scene lighting
        uniform vec3 uAmbientLight;

        // Directional lights
        struct DirectionalLight {
            vec3 direction;
            vec3 color;
            float intensity;
        };
        uniform int uNumDirectionalLights;
        uniform DirectionalLight uDirectionalLights[2];

        // Point lights
        struct PointLight {
            vec3 position;
            vec3 color;
            float intensity;
            float range;
            float attenuation;
        };
        uniform int uNumPointLights;
        uniform PointLight uPointLights[8];

        // Spot lights
        struct SpotLight {
            vec3 position;
            vec3 direction;
            vec3 color;
            float intensity;
            float range;
            float innerConeAngle;
            float outerConeAngle;
        };
        uniform int uNumSpotLights;
        uniform SpotLight uSpotLights[4];

        in vec3 fragNormal;
        in vec3 fragWorldPos;
        in vec2 fragTexCoord;

        out vec4 FragColor;

        float calculateAttenuation(float distance, float range, float attenuation) {
            float d = distance / range;
            return 1.0 / (1.0 + attenuation * d * d);
        }

        void main() {
            // Get base color
            vec4 albedo = uAlbedo;
            if (uHasTexture) {
                albedo *= texture(uTexture, fragTexCoord);
            }

            vec3 N = normalize(fragNormal);
            vec3 V = normalize(uCameraPos - fragWorldPos);

            // Start with ambient
            vec3 lighting = uAmbientLight;

            // Directional lights
            for (int i = 0; i < uNumDirectionalLights; i++) {
                vec3 L = normalize(-uDirectionalLights[i].direction);
                float NdotL = max(dot(N, L), 0.0);

                // Diffuse
                vec3 diffuse = uDirectionalLights[i].color *
                              uDirectionalLights[i].intensity *
                              NdotL;

                // Specular (Blinn-Phong)
                vec3 H = normalize(L + V);
                float NdotH = max(dot(N, H), 0.0);
                float shininess = mix(4.0, 256.0, uSmoothness);
                float spec = pow(NdotH, shininess);
                vec3 specular = uDirectionalLights[i].color * spec * 0.3;

                lighting += diffuse + specular;
            }

            // Point lights
            for (int i = 0; i < uNumPointLights; i++) {
                vec3 lightToFrag = fragWorldPos - uPointLights[i].position;
                float distance = length(lightToFrag);

                if (distance > uPointLights[i].range)
                    continue;

                vec3 L = normalize(-lightToFrag);
                float NdotL = max(dot(N, L), 0.0);
                float attenuation = calculateAttenuation(
                    distance,
                    uPointLights[i].range,
                    uPointLights[i].attenuation
                );

                vec3 diffuse = uPointLights[i].color *
                              uPointLights[i].intensity *
                              NdotL *
                              attenuation;

                vec3 H = normalize(L + V);
                float NdotH = max(dot(N, H), 0.0);
                float shininess = mix(4.0, 256.0, uSmoothness);
                float spec = pow(NdotH, shininess);
                vec3 specular = uPointLights[i].color * spec * 0.3 * attenuation;

                lighting += diffuse + specular;
            }

            // Spot lights
            for (int i = 0; i < uNumSpotLights; i++) {
                vec3 lightToFrag = fragWorldPos - uSpotLights[i].position;
                float distance = length(lightToFrag);

                if (distance > uSpotLights[i].range)
                    continue;

                vec3 L = normalize(-lightToFrag);

                // Cone attenuation
                float theta = dot(L, normalize(-uSpotLights[i].direction));
                float epsilon = uSpotLights[i].innerConeAngle - uSpotLights[i].outerConeAngle;
                float spotIntensity = clamp(
                    (theta - uSpotLights[i].outerConeAngle) / epsilon,
                    0.0,
                    1.0
                );

                if (spotIntensity == 0.0)
                    continue;

                float NdotL = max(dot(N, L), 0.0);
                float attenuation = calculateAttenuation(distance, uSpotLights[i].range, 1.0);

                vec3 diffuse = uSpotLights[i].color *
                              uSpotLights[i].intensity *
                              NdotL *
                              attenuation *
                              spotIntensity;

                vec3 H = normalize(L + V);
                float NdotH = max(dot(N, H), 0.0);
                float shininess = mix(4.0, 256.0, uSmoothness);
                float spec = pow(NdotH, shininess);
                vec3 specular = uSpotLights[i].color * spec * 0.3 * attenuation * spotIntensity;

                lighting += diffuse + specular;
            }

            FragColor = vec4(lighting * albedo.rgb, albedo.a);
        }
    )";

    m_standardLitShader = CreateShaderProgram(litVert, litFrag);

    if (!m_standardUnlitShader || !m_standardLitShader)
    {
        std::cerr << "Failed to create standard shaders!" << std::endl;
    }
}

Renderer::Common::IShader* OpenGLRenderer::GetStandardUnlitShader() const
{
    return m_standardUnlitShader;
}

Renderer::Common::IShader* OpenGLRenderer::GetStandardLitShader() const
{
    return m_standardLitShader;
}

// Factory function
std::unique_ptr<Renderer::Common::IRenderer> Renderer::OpenGL::CreateOpenGLRenderer()
{
    return std::make_unique<OpenGLRenderer>();
}
