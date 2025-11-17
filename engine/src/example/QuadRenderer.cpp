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
    _shader = renderer->GetStandardUnlitShader();

    // Create a simple quad (two triangles forming a square)
    Renderer::Common::MeshData quadData;

    // Vertex data - color values don't matter since we use uniform
    // Using white (1,1,1,1) as a default
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Quad vertices (centered at origin, 1x1 size)
    quadData.vertices = {
        // Bottom-left
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
        // Bottom-right
        {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
        // Top-right
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
        // Top-left
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}};

    // Two triangles: (0,1,2) and (0,2,3)
    quadData.indices = {0, 1, 2, 0, 2, 3};

    _mesh = renderer->CreateMesh(quadData);
    _material = renderer->CreateMaterial(_shader, 0); // No texture for now
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

    if (_mesh == 0)
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
    Positionable::Matrix4 scaleMatrix{Positionable::Matrix4::identity()};
    scaleMatrix(0, 0) = _size.x;
    scaleMatrix(1, 1) = _size.y;
    scaleMatrix(2, 2) = _size.z;

    // Combine: finalMatrix = worldMatrix * scaleMatrix
    Positionable::Matrix4 finalMatrix = worldMatrix * scaleMatrix;

    _material->SetColor("uAlbedo", _color.r, _color.g, _color.b, _color.a);

    // Pass the final matrix to the renderer
    renderer->DrawMesh(_mesh, finalMatrix.Data(), _material);
}

void QuadRenderer::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
{
    if (!_resourcesInitialized || !renderer)
        return;

    if (_mesh != 0)
    {
        renderer->DestroyMesh(_mesh);
        _mesh = 0;
    }

    if (_material != nullptr)
    {
        renderer->DestroyMaterial(_material);
        _material = nullptr;
    }

    if (_shader != 0)
    {
        renderer->DestroyShaderProgram(_shader); // Clean up the shader
        _shader = 0;
    }

    _resourcesInitialized = false;
}