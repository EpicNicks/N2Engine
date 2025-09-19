#include "engine/example/QuadRenderer.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"

#include <math/Matrix.hpp>

using namespace N2Engine;
using namespace N2Engine::Example;

QuadRenderer::QuadRenderer(GameObject &gameObject)
    : Component(gameObject), _color{Common::Color::White()}, _size{Math::Vector3::One()}
{
    _gameObject.CreatePositionable();
}

QuadRenderer::~QuadRenderer()
{
    // Resources should be cleaned up in OnDestroy
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
    _materialId = renderer->CreateMaterial(0); // No texture for now
}

void QuadRenderer::Render(Renderer::Common::IRenderer *renderer)
{
    if (!_resourcesInitialized || _meshId == 0 || !renderer)
        return;

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

    _resourcesInitialized = false;
}