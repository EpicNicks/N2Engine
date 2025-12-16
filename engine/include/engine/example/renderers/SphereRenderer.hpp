#pragma once

#include <math/Constants.hpp>

#include "engine/example/renderers/PolygonRenderer.hpp"

namespace N2Engine::Example
{
    /**
     * @brief Renders a UV sphere with configurable quality
     * 
     * The sphere is centered at the origin with a default size of 1x1x1.
     * Uses parametric equations to generate a smooth sphere mesh.
     * Quality can be controlled via SetSubdivision() before first render.
     */
    class SphereRenderer final : public PolygonRenderer<SphereRenderer>
    {
        friend class PolygonRenderer;

    private:
        // Sphere-specific quality settings
        // Sphere-specific quality settings
        uint32_t _latitudeSegments = 16; // Rings (vertical)
        uint32_t _longitudeSegments = 32; // Slices (horizontal)

        void CreateMesh(Renderer::Common::IRenderer *renderer)
        {
            // Generate UV sphere using parametric equations
            Renderer::Common::MeshData sphereData;

            // Generate vertices
            for (uint32_t lat = 0; lat <= _latitudeSegments; ++lat)
            {
                constexpr float PI = Math::Constants::PI_F;
                const float theta = static_cast<float>(lat) * PI / static_cast<float>(_latitudeSegments);
                // 0 to PI (top to bottom)
                const float sinTheta = std::sin(theta);
                const float cosTheta = std::cos(theta);

                for (uint32_t lon = 0; lon <= _longitudeSegments; ++lon)
                {
                    constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    constexpr float radius = 0.5f;
                    const float phi = static_cast<float>(lon) * 2.0f * PI / static_cast<float>(_longitudeSegments);
                    // 0 to 2*PI (around)
                    const float sinPhi = std::sin(phi);
                    const float cosPhi = std::cos(phi);

                    // Vertex position on unit sphere
                    const float x = cosPhi * sinTheta;
                    const float y = cosTheta;
                    const float z = sinPhi * sinTheta;

                    // UV texture coordinates
                    const float u = static_cast<float>(lon) / static_cast<float>(_longitudeSegments);
                    const float v = static_cast<float>(lat) / static_cast<float>(_latitudeSegments);

                    Renderer::Common::Vertex vertex;
                    vertex.position[0] = x * radius;
                    vertex.position[1] = y * radius;
                    vertex.position[2] = z * radius;

                    // Normal (same as normalized position for unit sphere)
                    vertex.normal[0] = x;
                    vertex.normal[1] = y;
                    vertex.normal[2] = z;

                    // Texture coordinates
                    vertex.texCoord[0] = u;
                    vertex.texCoord[1] = v;

                    // Color
                    vertex.color[0] = white[0];
                    vertex.color[1] = white[1];
                    vertex.color[2] = white[2];
                    vertex.color[3] = white[3];

                    sphereData.vertices.push_back(vertex);
                }
            }

            // Generate indices (two triangles per quad)
            for (uint32_t lat = 0; lat < _latitudeSegments; ++lat)
            {
                for (uint32_t lon = 0; lon < _longitudeSegments; ++lon)
                {
                    const uint32_t first = lat * (_longitudeSegments + 1) + lon;
                    const uint32_t second = first + _longitudeSegments + 1;

                    // First triangle
                    sphereData.indices.push_back(first);
                    sphereData.indices.push_back(second);
                    sphereData.indices.push_back(first + 1);

                    // Second triangle
                    sphereData.indices.push_back(second);
                    sphereData.indices.push_back(second + 1);
                    sphereData.indices.push_back(first + 1);
                }
            }

            _mesh = renderer->CreateMesh(sphereData);
            _material = renderer->CreateMaterial(_shader, nullptr);
        }

    public:
        explicit SphereRenderer(GameObject &gameObject) : PolygonRenderer(gameObject)
        {
            // Register sphere-specific parameters
            RegisterMember(NAMEOF(_latitudeSegments), _latitudeSegments);
            RegisterMember(NAMEOF(_longitudeSegments), _longitudeSegments);
        }

        [[nodiscard]] std::string GetTypeName() const override
        {
            return NAMEOF(SphereRenderer);
        }

        // Sphere-specific: control mesh quality (must call before first render!)
        void SetSubdivision(const uint32_t latitude, const uint32_t longitude)
        {
            _latitudeSegments = latitude;
            _longitudeSegments = longitude;
        }

        void SetRadius(const float radius)
        {
            _size = Math::Vector3(radius);
        }

        [[nodiscard]] uint32_t GetLatitudeSegments() const { return _latitudeSegments; }
        [[nodiscard]] uint32_t GetLongitudeSegments() const { return _longitudeSegments; }
    };
}
