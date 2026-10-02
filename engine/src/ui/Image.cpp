#include "engine/ui/Image.hpp"

#include <renderer/common/RenderTypes.hpp>

namespace N2Engine::UI
{
    Image::Image(GameObject &gameObject) : UIGraphic(gameObject) {}

    Math::Matrix<float, 4, 4> Image::ModelMatrixFor(const Rect &rect)
    {
        // Scale the unit square to the rect's size, then move its corner to the rect's corner
        Math::Matrix<float, 4, 4> model; // zero-filled
        model(0, 0) = rect.width;
        model(1, 1) = rect.height;
        model(2, 2) = 1.0f;
        model(3, 3) = 1.0f;
        model(0, 3) = rect.x;
        model(1, 3) = rect.y;
        return model;
    }

    bool Image::EnsureResources(Renderer::Common::IRenderer *renderer)
    {
        if (_mesh && _material)
        {
            return true;
        }
        _renderer = renderer;
        _rendererLifetime = renderer->GetLifetimeToken();

        if (!_mesh)
        {
            // The unit square, counter-clockwise seen from +z (front-facing in canvas space, which is y up).
            // The UI pass draws with culling off anyway.
            Renderer::Common::MeshData quad;
            quad.vertices = {
                {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            };
            quad.indices = {0, 1, 2, 0, 2, 3};
            _mesh = renderer->CreateMesh(quad);
        }
        if (!_material)
        {
            // The renderer owns its standard shader; the Image never destroys it
            if (Renderer::Common::IShader *shader = renderer->GetStandardUnlitShader())
            {
                _material = renderer->CreateMaterial(shader, nullptr);
            }
        }
        return _mesh && _material;
    }

    void Image::ReleaseResources()
    {
        // A destroyed renderer freed them itself, and its address may now be another renderer's
        if (_renderer && !_rendererLifetime.expired())
        {
            if (_mesh)
            {
                _renderer->DestroyMesh(_mesh);
            }
            if (_material)
            {
                _renderer->DestroyMaterial(_material);
            }
        }
        _mesh = nullptr;
        _material = nullptr;
        _renderer = nullptr;
        _rendererLifetime.reset();
    }

    void Image::RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                         const Renderer::Common::RenderState &state)
    {
        if (!renderer || !rect.HasArea())
        {
            return;
        }
        if (_renderer && (_renderer != renderer || _rendererLifetime.expired()))
        {
            // Drawn by another renderer than the one that made the quad, or by a new renderer at the old one's
            // address (the window was re-created): start again with this one. The old resources are destroyed
            // only if their renderer still exists.
            ReleaseResources();
        }
        if (!EnsureResources(renderer))
        {
            return;
        }

        _material->SetTexture(_texture);
        _material->SetInt("uHasTexture", _texture != nullptr ? 1 : 0);
        _material->SetColor("uAlbedo", _color.r, _color.g, _color.b, _color.a);

        const Math::Matrix<float, 4, 4> model = ModelMatrixFor(rect);
        renderer->DrawMesh(_mesh, model.Data(), _material, state);
    }

    void Image::OnDestroy()
    {
        ReleaseResources();
    }
}
