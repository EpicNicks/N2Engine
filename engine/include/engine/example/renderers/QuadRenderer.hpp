#pragma once

#include "engine/example/renderers/PolygonRenderer.hpp"

namespace N2Engine::Example
{
    /**
     * @brief Renders a 2D quad: the built-in Quad mesh (Rendering::BuiltinMesh::Quad)
     *
     * The quad is centered at the origin with a default size of 1x1, on the x/y plane, facing +Z (its back is
     * culled). Every QuadRenderer on a renderer shares one GPU mesh.
     */
    class QuadRenderer final : public PolygonRenderer
    {
    public:
        explicit QuadRenderer(GameObject &gameObject) : PolygonRenderer(gameObject)
        {
        }

        [[nodiscard]] std::string GetTypeName() const override
        {
            return NAMEOF(QuadRenderer);
        }

        [[nodiscard]] std::shared_ptr<Rendering::Mesh> GetMesh() override
        {
            return Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Quad);
        }
    };
}
