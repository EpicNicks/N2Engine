#include "engine/example/QuadRenderer.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"

#include <math/Matrix.hpp>
#include <string>
#include "engine/Application.hpp"

using namespace N2Engine;
using namespace N2Engine::Example;

QuadRenderer::QuadRenderer(GameObject &gameObject)
    : IRenderable(gameObject), _color{Common::Color::White()}, _size{Math::Vector3::One()}
{
    _gameObject.CreatePositionable();
}

QuadRenderer::~QuadRenderer()
{
    // Resources should be cleaned up in OnDestroy
}

void QuadRenderer::OnUpdate()
{
    // Logger::Info("delta time: " + std::to_string(Time::GetDeltaTime()));
    // Logger::Info("time: " + std::to_string(Time::GetTime()));
    auto mainCamera = Application::GetInstance().GetMainCamera();
    // mainCamera->SetPosition(_gameObject.GetPositionable()->GetPosition() + Math::Vector3{0.0f, 0.0f, 5.0f});

    static float totalTime = 0.0f;
    totalTime += Time::GetDeltaTime();

    // Rotate the quad around Y axis
    float angle = totalTime * 1.0f; // 1 radian per second

    auto positionable = _gameObject.GetPositionable();
    if (positionable)
    {
        // Set rotation using quaternion (assuming you have this method)
        positionable->SetRotation(Math::Quaternion::FromEulerAngles(0.0f, angle, 0.0f));

        // Also try a slight wobble on position to make it more visible
        float wobbleX = sin(totalTime * 2.0f) * 0.5f;
        float wobbleY = cos(totalTime * 3.0f) * 0.3f;
        positionable->SetPosition(Math::Vector3{wobbleX, wobbleY, 0.0f});
    }

    // _gameObject.GetPositionable()->SetScale(_gameObject.GetPositionable()->GetScale() + Math::Vector3::CreateUniform(Time::GetDeltaTime()));
    // Logger::Info("scale: " + _gameObject.GetPositionable()->GetScale().toString());
}

void QuadRenderer::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
{
    if (_resourcesInitialized || !renderer)
        return;

    CreateQuadMesh(renderer);
    _resourcesInitialized = true;
}

void QuadRenderer::CreateQuadMesh(Renderer::Common::IRenderer *renderer)
{
    const char *vertexShader =
        "#version 330 core\n"
        "layout (location = 0) in vec3 aPos;\n"
        "layout (location = 1) in vec3 aNormal;\n"
        "layout (location = 2) in vec2 aTexCoord;\n"
        "layout (location = 3) in vec4 aColor;\n"
        "uniform mat4 uModel;\n"
        "uniform mat4 uView;\n"
        "uniform mat4 uProjection;\n"
        "out vec4 vertexColor;\n"
        "void main() {\n"
        "    gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);\n"
        "    vertexColor = aColor;\n"
        "}";

    const char *fragmentShader =
        "#version 330 core\n"
        "in vec4 vertexColor;\n"
        "out vec4 FragColor;\n"
        "void main() {\n"
        "    FragColor = vertexColor;\n"
        "}";

    _shaderId = renderer->CreateShaderProgram(vertexShader, fragmentShader);

    // Create a simple quad (two triangles forming a square)
    Renderer::Common::MeshData quadData;

    // Quad vertices (centered at origin, 1x1 size)
    // Normal is now {0.0f, 0.0f, -1.0f} to face toward -Z (toward camera)
    // quadData.vertices = {
    //     // Bottom-left
    //     {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {_color.r, _color.g, _color.b, _color.a}},
    //     // Bottom-right
    //     {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {_color.r, _color.g, _color.b, _color.a}},
    //     // Top-right
    //     {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {_color.r, _color.g, _color.b, _color.a}},
    //     // Top-left
    //     {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {_color.r, _color.g, _color.b, _color.a}}};

    quadData.vertices = {
        // Positions in clip space - this will fill screen
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}};

    // Two triangles: (0,1,2) and (0,2,3)
    quadData.indices = {0, 1, 2, 0, 2, 3};

    _meshId = renderer->CreateMesh(quadData);
    _materialId = renderer->CreateMaterial(_shaderId, 0); // No texture for now
}

void QuadRenderer::Render(Renderer::Common::IRenderer *renderer)
{
    if (!renderer)
    {
        return;
    }

    // Initialize resources if not already done
    if (!_resourcesInitialized)
    {
        InitializeRenderResources(renderer);
        if (!_resourcesInitialized) // Check if initialization failed
        {
            return;
        }
    }

    if (_meshId == 0)
    {
        return;
    }

    const GameObject &gameObject = GetGameObject();

    std::shared_ptr<N2Engine::Positionable> positionable = gameObject.GetPositionable();
    if (!positionable)
        return;

    // Get the world transform matrix (this handles hierarchy automatically)
    Positionable::Matrix4 worldMatrix = positionable->GetLocalToWorldMatrix();

    // Apply the quad's size scaling to the transform
    // We need to scale the transform by our size
    Positionable::Matrix4 scaleMatrix{Positionable::Matrix4::identity()};
    scaleMatrix(0, 0) = _size.x;
    scaleMatrix(1, 1) = _size.y;
    scaleMatrix(2, 2) = _size.z;

    // Combine: finalMatrix = worldMatrix * scaleMatrix
    Positionable::Matrix4 finalMatrix = worldMatrix * scaleMatrix;

    // Logger::Info("\n" + finalMatrix.toString());

    // Pass the final matrix to the renderer
    renderer->DrawMesh(_meshId, finalMatrix.data.data(), _materialId);
}

void QuadRenderer::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
{
    if (!_resourcesInitialized || !renderer)
        return;

    if (_meshId != 0)
    {
        renderer->DestroyMesh(_meshId);
        _meshId = 0;
    }

    if (_materialId != 0)
    {
        renderer->DestroyMaterial(_materialId);
        _materialId = 0;
    }

    if (_shaderId != 0)
    {
        renderer->DestroyShaderProgram(_shaderId); // Clean up the shader
        _shaderId = 0;
    }

    _resourcesInitialized = false;
}