#pragma once

#include <cstdint>
#include <memory>

#include "engine/example/renderers/PolygonRenderer.hpp"

namespace N2Engine::Example
{
    /**
     * @brief Renders a UV sphere with configurable quality
     *
     * The sphere is centered at the origin with a default size of 1x1x1. At the default subdivision (16 rings,
     * 32 slices) it draws the built-in Sphere mesh (Rendering::BuiltinMesh::Sphere), shared by every such sphere on
     * a renderer; other subdivisions make a mesh of their own, again whenever the subdivision changes.
     */
    class SphereRenderer final : public PolygonRenderer
    {
    private:
        // Sphere-specific quality settings
        uint32_t _latitudeSegments = 16; // Rings (vertical)
        uint32_t _longitudeSegments = 32; // Slices (horizontal)

        // The mesh for a subdivision other than the built-in one, and the subdivision it was made for (it stays
        // null if that subdivision can't make a mesh, so it isn't retried every frame)
        std::shared_ptr<Rendering::Mesh> _customMesh;
        uint32_t _customLatitude = 0;
        uint32_t _customLongitude = 0;
        bool _customMade = false;

    public:
        static constexpr uint32_t DefaultLatitudeSegments = 16;
        static constexpr uint32_t DefaultLongitudeSegments = 32;

        explicit SphereRenderer(GameObject &gameObject) : PolygonRenderer(gameObject)
        {
            // Register sphere-specific parameters
            RegisterMember(NAMEOF(_latitudeSegments), _latitudeSegments).Range(3.0, 256.0);
            RegisterMember(NAMEOF(_longitudeSegments), _longitudeSegments).Range(3.0, 256.0);
        }

        [[nodiscard]] std::string GetTypeName() const override
        {
            return NAMEOF(SphereRenderer);
        }

        [[nodiscard]] std::shared_ptr<Rendering::Mesh> GetMesh() override
        {
            if (_latitudeSegments == DefaultLatitudeSegments && _longitudeSegments == DefaultLongitudeSegments)
            {
                return Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Sphere);
            }
            if (!_customMade || _customLatitude != _latitudeSegments || _customLongitude != _longitudeSegments)
            {
                _customMesh = Rendering::Mesh::Create(Rendering::Mesh::MakeSphere(_latitudeSegments, _longitudeSegments));
                _customLatitude = _latitudeSegments;
                _customLongitude = _longitudeSegments;
                _customMade = true;
            }
            return _customMesh;
        }

        /// Sets the mesh quality; takes effect on the next draw
        void SetSubdivision(const uint32_t latitude, const uint32_t longitude)
        {
            _latitudeSegments = latitude;
            _longitudeSegments = longitude;
        }

        void SetRadius(const float radius)
        {
            _size = Math::Vector3(radius);
        }

        [[nodiscard]] float GetRadius() const { return _size.x; }

        [[nodiscard]] uint32_t GetLatitudeSegments() const { return _latitudeSegments; }
        [[nodiscard]] uint32_t GetLongitudeSegments() const { return _longitudeSegments; }
    };
}
