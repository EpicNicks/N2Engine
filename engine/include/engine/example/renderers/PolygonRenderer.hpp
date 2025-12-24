#pragma once

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IShader.hpp>

#include "engine/Component.hpp"
#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"

#include "engine/serialization/MathSerialization.hpp"

#include <concepts>
#include <type_traits>

namespace N2Engine::Example
{
    /**
     * @brief CRTP base class for polygon-based renderers (Quad, Cube, Sphere, etc.)
     *
     * This template uses the Curiously Recurring Template Pattern to provide:
     * - Zero-overhead abstraction (no virtual dispatch)
     * - Code reuse for common rendering logic
     * - Compile-time type safety
     *
     * Derived classes must implement:
     * - CreateMesh(Renderer::Common::IRenderer* renderer)
     * - GetTypeName() const
     *
     * @tparam Derived The derived renderer class (e.g., QuadRenderer)
     *
     * Example:
     * @code
     * class QuadRenderer final : public PolygonRenderer<QuadRenderer> {
     * protected:
     *     void CreateMesh(Renderer::Common::IRenderer* renderer) {
     *         // Generate quad mesh...
     *     }
     * };
     * @endcode
     */
    template<typename Derived>
    class PolygonRenderer : public IRenderable
    {
    protected:
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;
        Renderer::Common::IShader *_shader = nullptr;
        bool _resourcesInitialized = false;

        Renderer::Common::IRenderer *_cachedRenderer = nullptr;

        // Rendering properties (common to all polygon renderers)
        Common::Color _color{Common::Color::White};
        Math::Vector3 _size{Math::Vector3::One};

    public:
        explicit PolygonRenderer(GameObject& gameObject) : IRenderable(gameObject)
        {
            _gameObject.CreatePositionable();
            RegisterMember(NAMEOF(_color), _color);
            RegisterMember(NAMEOF(_size), _size);
        }

        void OnDestroy() override
        {
            CleanupRenderResources(_cachedRenderer);
        }

        void InitializeRenderResources(Renderer::Common::IRenderer* renderer) override
        {
            if (_resourcesInitialized || !renderer)
            {
                return;
            }

            // Compile-time check that Derived implements CreateMesh
            // This fires when Derived is complete, giving a clear error message
            static_assert(requires(Derived& d, Renderer::Common::IRenderer* r) {
                { d.CreateMesh(r) } -> std::same_as<void>;
            }, "Derived class must implement: void CreateMesh(Renderer::Common::IRenderer*)");

            _cachedRenderer = renderer;
            _shader = renderer->GetStandardUnlitShader();

            // CRTP: Static dispatch to derived class's CreateMesh
            static_cast<Derived*>(this)->CreateMesh(renderer);
            
            _resourcesInitialized = true;
        }

        void Render(Renderer::Common::IRenderer* renderer) override
        {
            if (!renderer)
            {
                return;
            }

            // Initialize resources if not already done
            if (!_resourcesInitialized)
            {
                InitializeRenderResources(renderer);
                if (!_resourcesInitialized)
                {
                    return;
                }
            }

            if (_mesh == nullptr)
            {
                return;
            }

            const GameObject& gameObject = GetGameObject();
            const Positionable* positionable = gameObject.GetPositionable();
            if (!positionable)
                return;

            // Get the world transform matrix (this handles hierarchy automatically)
            const Positionable::Matrix4 worldMatrix = positionable->GetLocalToWorldMatrix();

            // Apply the polygon's size scaling to the transform
            Positionable::Matrix4 scaleMatrix{Positionable::Matrix4::identity()};
            scaleMatrix(0, 0) = _size.x;
            scaleMatrix(1, 1) = _size.y;
            scaleMatrix(2, 2) = _size.z;

            // Combine: finalMatrix = worldMatrix * scaleMatrix
            Positionable::Matrix4 finalMatrix = worldMatrix * scaleMatrix;

            // Set color uniform
            _material->SetColor("uAlbedo", _color.r, _color.g, _color.b, _color.a);

            // Pass the final matrix to the renderer
            renderer->DrawMesh(_mesh, finalMatrix.Data(), _material);
        }

        void CleanupRenderResources(Renderer::Common::IRenderer* renderer) override
        {
            if (!_resourcesInitialized || !renderer)
                return;

            if (_mesh != nullptr)
            {
                renderer->DestroyMesh(_mesh);
                _mesh = nullptr;
            }

            if (_material != nullptr)
            {
                renderer->DestroyMaterial(_material);
                _material = nullptr;
            }

            if (_shader != nullptr)
            {
                renderer->DestroyShaderProgram(_shader);
                _shader = nullptr;
            }

            _resourcesInitialized = false;
        }

        // Common properties
        void SetColor(const Common::Color& color) { _color = color; }
        [[nodiscard]] const Common::Color& GetColor() const { return _color; }

        void SetSize(const Math::Vector3& size) { _size = size; }
        [[nodiscard]] const Math::Vector3& GetSize() const { return _size; }

        static constexpr bool IsSingleton = false;
    };
}