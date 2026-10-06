#pragma once

#include "engine/example/renderers/PolygonRenderer.hpp"

namespace N2Engine::Example
{
    /**
     * @brief Renders a 3D cube: the built-in Cube mesh (Rendering::BuiltinMesh::Cube)
     *
     * The cube is centered at the origin with a default size of 1x1x1. It has 24 vertices (4 per face) so each
     * face has its own normal. Every CubeRenderer on a renderer shares one GPU mesh.
     */
    class CubeRenderer final : public PolygonRenderer
    {
    public:
        explicit CubeRenderer(GameObject &gameObject) : PolygonRenderer(gameObject)
        {
        }

        [[nodiscard]] std::string GetTypeName() const override
        {
            return NAMEOF(CubeRenderer);
        }

        [[nodiscard]] std::shared_ptr<Rendering::Mesh> GetMesh() override
        {
            return Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube);
        }
    };
}
