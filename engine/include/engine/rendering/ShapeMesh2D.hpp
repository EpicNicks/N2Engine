#pragma once

// The meshes Sprite2D draws (docs/sprite2d.html): flat shapes in the x, y plane, facing +Z, built on the CPU. A pure function and a
// cache, so they are tested without a renderer and a thousand sprites of one shape share one mesh.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/rendering/Mesh.hpp"

namespace N2Engine::Rendering
{
    enum class Shape2D : std::uint8_t
    {
        Rect,
        Triangle, ///< pointing up: the base along the bottom of the size, the apex at the top middle
        Circle,   ///< an ellipse when the size is not square
        RegularPolygon,
        Polygon   ///< convex, its points in units of the size
    };

    struct ShapeDesc2D
    {
        Shape2D shape = Shape2D::Rect;
        Math::Vector2 size{1.0f, 1.0f};
        int sides = 6;
        std::vector<Math::Vector2> vertices;
        /// 0 fills; above 0, a border this wide in world units, the same width on every side
        float outline = 0.0f;
        /// The part of the texture used, in uv (u right, v up, as the engine's quad): its lower-left corner and its size
        Math::Vector2 regionOffset{0.0f, 0.0f};
        Math::Vector2 regionSize{1.0f, 1.0f};
        /// Where the origin sits within the size: (0.5, 0.5) is the middle
        Math::Vector2 pivot{0.5f, 0.5f};
    };

    /// Segments of a circle: its edge strays under 1% of the radius from the true curve
    inline constexpr int kCircleSegments2D = 32;
    inline constexpr int kMaxSides2D = 64;

    /// The outline points of a shape, counter-clockwise, in world units around the pivot. Empty for a shape that cannot be drawn
    /// (a size not above zero, sides out of 3 to 64, a polygon that is not convex or has under 3 points); `error` then says why.
    [[nodiscard]] std::vector<Math::Vector2> ShapeOutline2D(const ShapeDesc2D &desc, std::string *error = nullptr);

    /// The mesh data: a fan for a filled shape, a ring of quads for an outline (an outline at least as wide as the shape is
    /// filled). Empty when ShapeOutline2D is.
    [[nodiscard]] Renderer::Common::MeshData MakeShapeMesh2D(const ShapeDesc2D &desc, std::string *error = nullptr);

    /// The mesh for a description, shared while anything uses it. Null, with a warning logged once per description, for a shape
    /// that cannot be drawn.
    [[nodiscard]] std::shared_ptr<Mesh> GetShapeMesh2D(const ShapeDesc2D &desc);
}
