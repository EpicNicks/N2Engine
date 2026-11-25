#pragma once

#include <optional>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>

#include "engine/ui/Rect.hpp"

namespace N2Engine::UI
{
    /**
     * A 2D affine transform of canvas space (issue #445, RectTransform rotation and scale), with column vectors as the
     * engine's matrices: a point p goes to (a * x + c * y + tx, b * x + d * y + ty). The default is the identity.
     */
    struct Transform2D
    {
        float a = 1.0f;
        float b = 0.0f;
        float c = 0.0f;
        float d = 1.0f;
        float tx = 0.0f;
        float ty = 0.0f;

        /**
         * Scale, then rotation (degrees, counter-clockwise on screen, as canvas space is y up), both about `pivot`.
         * Whole quarter turns are exact. A non-finite angle counts as 0 and a non-finite scale component as 1. Exactly
         * the identity for no rotation and a scale of 1.
         */
        [[nodiscard]] static Transform2D AroundPivot(const Math::Vector2 &pivot, float degrees, const Math::Vector2 &scale);

        [[nodiscard]] constexpr bool IsIdentity() const { return *this == Transform2D{}; }

        [[nodiscard]] Math::Vector2 Apply(const Math::Vector2 &point) const
        {
            return Math::Vector2{a * point.x + c * point.y + tx, b * point.x + d * point.y + ty};
        }
        /// Apply without the translation: for a direction or a difference of two points
        [[nodiscard]] Math::Vector2 ApplyVector(const Math::Vector2 &vector) const
        {
            return Math::Vector2{a * vector.x + c * vector.y, b * vector.x + d * vector.y};
        }

        /// `inner` first, then this transform
        [[nodiscard]] Transform2D operator*(const Transform2D &inner) const;

        /// Nothing for a transform that flattens the plane (a zero scale), which has no inverse
        [[nodiscard]] std::optional<Transform2D> Inverse() const;

        /// The same transform as a 4 x 4 matrix (row-major, z unchanged), for a draw's model matrix
        [[nodiscard]] Math::Matrix<float, 4, 4> ToMatrix() const;

        friend constexpr bool operator==(const Transform2D &, const Transform2D &) = default;
    };

    /// The corners of a convex region, counter-clockwise (y up)
    using ConvexPolygon = std::vector<Math::Vector2>;

    /// The four corners of `rect` through `transform`, counter-clockwise even under a mirroring scale
    [[nodiscard]] ConvexPolygon TransformedCorners(const Rect &rect, const Transform2D &transform);

    /// The overlap of two convex counter-clockwise polygons (Sutherland-Hodgman); empty when they miss or only touch
    [[nodiscard]] ConvexPolygon IntersectConvex(const ConvexPolygon &subject, const ConvexPolygon &clip);

    /// Whether `point` is inside a convex counter-clockwise polygon, edges included; false for fewer than 3 corners
    [[nodiscard]] bool ConvexContains(const ConvexPolygon &polygon, const Math::Vector2 &point);

    /// The area of a counter-clockwise polygon (0 for fewer than 3 corners)
    [[nodiscard]] float PolygonArea(const ConvexPolygon &polygon);

    /// The smallest axis-aligned rect around the points (zeros for none)
    [[nodiscard]] Rect BoundsOf(const ConvexPolygon &polygon);

    /// The rect a polygon is, when it is one with its sides along the axes (within `tolerance` canvas units)
    [[nodiscard]] std::optional<Rect> AsAxisAlignedRect(const ConvexPolygon &polygon, float tolerance = 1e-3f);
}
