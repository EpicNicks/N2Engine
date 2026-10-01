#pragma once

#include "engine/example/renderers/PolygonRenderer.hpp"

namespace N2Engine::Example
{
    /**
     * @brief Renders a 2D quad (two triangles forming a square)
     *
     * The quad is centered at the origin with a default size of 1x1.
     * Uses the PolygonRenderer CRTP base for common rendering logic.
     */
    class QuadRenderer final : public PolygonRenderer<QuadRenderer>
    {
        friend class PolygonRenderer;

    private:
        void CreateMesh(Renderer::Common::IRenderer* renderer)
        {
            // Create a simple quad (two triangles forming a square)
            Renderer::Common::MeshData quadData;

            constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

            // Quad vertices (centered at origin, 1x1 size)
            quadData.vertices = {
                // Bottom-left
                {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                // Bottom-right
                {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                // Top-right
                {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                // Top-left
                {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}
            };

            // Two triangles: (0,1,2) and (0,2,3)
            quadData.indices = {0, 1, 2, 0, 2, 3};

            _mesh = renderer->CreateMesh(quadData);
            _material = renderer->CreateMaterial(_shader, nullptr);
        }

    public:
        explicit QuadRenderer(GameObject& gameObject): PolygonRenderer(gameObject)
        {
        }

        [[nodiscard]] std::string GetTypeName() const override
        {
            return NAMEOF(QuadRenderer);
        }
    };
}