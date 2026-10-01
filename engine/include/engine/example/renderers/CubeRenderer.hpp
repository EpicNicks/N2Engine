#pragma once

#include "engine/example/renderers/PolygonRenderer.hpp"

namespace N2Engine::Example
{
    /**
     * @brief Renders a 3D cube with proper per-face normals for lighting
     *
     * The cube is centered at the origin with a default size of 1x1x1.
     * Uses 24 vertices (4 per face) to ensure correct normals on each face.
     */
    class CubeRenderer final : public PolygonRenderer<CubeRenderer>
    {
        friend class PolygonRenderer;

    private:
        void CreateMesh(Renderer::Common::IRenderer* renderer)
        {
            // Create cube mesh (unit cube, 24 vertices for proper normals)
            Renderer::Common::MeshData cubeData;

            constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            constexpr float h = 0.5f; // Half size for unit cube

            // 24 vertices (4 per face × 6 faces) for proper per-face normals
            cubeData.vertices = {
                // Front face (Z+)
                {{-h, -h,  h}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h, -h,  h}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h,  h,  h}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h,  h,  h}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

                // Back face (Z-)
                {{ h, -h, -h}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h, -h, -h}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h,  h, -h}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h,  h, -h}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

                // Right face (X+)
                {{ h, -h,  h}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h, -h, -h}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h,  h, -h}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h,  h,  h}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

                // Left face (X-)
                {{-h, -h, -h}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h, -h,  h}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h,  h,  h}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h,  h, -h}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

                // Top face (Y+)
                {{-h,  h,  h}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h,  h,  h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h,  h, -h}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h,  h, -h}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

                // Bottom face (Y-)
                {{-h, -h, -h}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h, -h, -h}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
                {{ h, -h,  h}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
                {{-h, -h,  h}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}
            };

            // 36 indices (6 faces × 2 triangles × 3 vertices)
            cubeData.indices = {
                // Front
                0, 1, 2, 0, 2, 3,
                // Back
                4, 5, 6, 4, 6, 7,
                // Right
                8, 9, 10, 8, 10, 11,
                // Left
                12, 13, 14, 12, 14, 15,
                // Top
                16, 17, 18, 16, 18, 19,
                // Bottom
                20, 21, 22, 20, 22, 23
            };

            _mesh = renderer->CreateMesh(cubeData);
            _material = renderer->CreateMaterial(_shader, nullptr);
        }

    public:
        explicit CubeRenderer(GameObject& gameObject) : PolygonRenderer(gameObject)
        {
        }

        [[nodiscard]] std::string GetTypeName() const override
        {
            return NAMEOF(CubeRenderer);
        }
    };
}