#include "engine/example/QuadRenderer.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"
#include "engine/Application.hpp"
#include "engine/serialization/MathSerialization.hpp"

#include <math/Matrix.hpp>
#include <string>

using namespace N2Engine;
using namespace N2Engine::Example;

QuadRenderer::QuadRenderer(GameObject &gameObject)
    : IRenderable(gameObject), _color{Common::Color::White()}, _size{Math::Vector3::One()}
{
    _gameObject.CreatePositionable();
    RegisterMember(NAMEOF(_color), _color);
    RegisterMember(NAMEOF(_size), _size);
}

void QuadRenderer::OnDestroy()
{
    CleanupRenderResources(_cachedRenderer);
}

void QuadRenderer::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
{
    if (_resourcesInitialized || !renderer)
    {
        return;
    }

    _cachedRenderer = renderer;

    CreateQuadMesh(renderer);
    _resourcesInitialized = true;
}

void QuadRenderer::CreateQuadMesh(Renderer::Common::IRenderer *renderer)
{
    const char *vertexShader = R"(
        #version 330 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aNormal;
        layout (location = 2) in vec2 aTexCoord;
        layout (location = 3) in vec4 aColor;
        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;
        out vec4 vertexColor;
        void main() {
            gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0);
            vertexColor = aColor;
        }
        )";

    const char *fragmentShader = R"(
        #version 330 core
        in vec4 vertexColor;
        out vec4 FragColor;
        void main() {
            FragColor = vertexColor;
        }
        )";

    _shaderId = renderer->CreateShaderProgram(vertexShader, fragmentShader);

    // Create a simple quad (two triangles forming a square)
    Renderer::Common::MeshData quadData;

    // Quad vertices (centered at origin, 1x1 size)
    quadData.vertices = {
        // Bottom-left
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {_color.r, _color.g, _color.b, _color.a}},
        // Bottom-right
        {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {_color.r, _color.g, _color.b, _color.a}},
        // Top-right
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {_color.r, _color.g, _color.b, _color.a}},
        // Top-left
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {_color.r, _color.g, _color.b, _color.a}}};

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

    // Pass the final matrix to the renderer
    renderer->DrawMesh(_meshId, finalMatrix.Data(), _materialId);
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