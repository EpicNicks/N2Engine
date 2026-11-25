#include "engine/ui/Transform2D.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include <math/DeterministicMath.hpp>

namespace N2Engine::UI
{
    namespace
    {
        /// Twice the signed area of the triangle (from, to, point): positive when the point is left of from -> to
        float Side(const Math::Vector2 &from, const Math::Vector2 &to, const Math::Vector2 &point)
        {
            return (to.x - from.x) * (point.y - from.y) - (to.y - from.y) * (point.x - from.x);
        }

        /// A polygon smaller than this (square canvas units) is nothing: two shapes that only touch
        constexpr float MinArea = 1e-6f;
    }

    Transform2D Transform2D::AroundPivot(const Math::Vector2 &pivot, const float degrees, const Math::Vector2 &scale)
    {
        const float angle = std::isfinite(degrees) ? std::fmod(degrees, 360.0f) : 0.0f;
        const float sx = std::isfinite(scale.x) ? scale.x : 1.0f;
        const float sy = std::isfinite(scale.y) ? scale.y : 1.0f;
        if (angle == 0.0f && sx == 1.0f && sy == 1.0f)
        {
            return Transform2D{};
        }
        // Quarter turns exactly, so a rect turned by 90 degrees stays along the axes
        float sine = 0.0f;
        float cosine = 1.0f;
        const float turn = angle < 0.0f ? angle + 360.0f : angle;
        if (turn == 90.0f)
        {
            sine = 1.0f;
            cosine = 0.0f;
        }
        else if (turn == 180.0f)
        {
            cosine = -1.0f;
        }
        else if (turn == 270.0f)
        {
            sine = -1.0f;
            cosine = 0.0f;
        }
        else if (turn != 0.0f && turn != 360.0f)
        {
            const float radians = angle * (std::numbers::pi_v<float> / 180.0f);
            sine = Math::Sin(radians);
            cosine = Math::Cos(radians);
        }
        Transform2D result;
        result.a = cosine * sx;
        result.b = sine * sx;
        result.c = -sine * sy;
        result.d = cosine * sy;
        // The pivot stays where it is
        result.tx = pivot.x - (result.a * pivot.x + result.c * pivot.y);
        result.ty = pivot.y - (result.b * pivot.x + result.d * pivot.y);
        return result;
    }

    Transform2D Transform2D::operator*(const Transform2D &inner) const
    {
        Transform2D result;
        result.a = a * inner.a + c * inner.b;
        result.b = b * inner.a + d * inner.b;
        result.c = a * inner.c + c * inner.d;
        result.d = b * inner.c + d * inner.d;
        result.tx = a * inner.tx + c * inner.ty + tx;
        result.ty = b * inner.tx + d * inner.ty + ty;
        return result;
    }

    std::optional<Transform2D> Transform2D::Inverse() const
    {
        if (IsIdentity())
        {
            return *this;
        }
        const float determinant = a * d - b * c;
        if (determinant == 0.0f || !std::isfinite(determinant))
        {
            return std::nullopt;
        }
        Transform2D result;
        result.a = d / determinant;
        result.b = -b / determinant;
        result.c = -c / determinant;
        result.d = a / determinant;
        result.tx = -(result.a * tx + result.c * ty);
        result.ty = -(result.b * tx + result.d * ty);
        if (!std::isfinite(result.a) || !std::isfinite(result.b) || !std::isfinite(result.c) ||
            !std::isfinite(result.d) || !std::isfinite(result.tx) || !std::isfinite(result.ty))
        {
            return std::nullopt;
        }
        return result;
    }

    Math::Matrix<float, 4, 4> Transform2D::ToMatrix() const
    {
        Math::Matrix<float, 4, 4> matrix = Math::Matrix<float, 4, 4>::identity();
        matrix(0, 0) = a;
        matrix(0, 1) = c;
        matrix(0, 3) = tx;
        matrix(1, 0) = b;
        matrix(1, 1) = d;
        matrix(1, 3) = ty;
        return matrix;
    }

    ConvexPolygon TransformedCorners(const Rect &rect, const Transform2D &transform)
    {
        ConvexPolygon corners{
            transform.Apply(Math::Vector2{rect.XMin(), rect.YMin()}),
            transform.Apply(Math::Vector2{rect.XMax(), rect.YMin()}),
            transform.Apply(Math::Vector2{rect.XMax(), rect.YMax()}),
            transform.Apply(Math::Vector2{rect.XMin(), rect.YMax()}),
        };
        if (transform.a * transform.d - transform.b * transform.c < 0.0f)
        {
            std::swap(corners[1], corners[3]); // mirrored: keep the winding counter-clockwise
        }
        return corners;
    }

    float PolygonArea(const ConvexPolygon &polygon)
    {
        if (polygon.size() < 3)
        {
            return 0.0f;
        }
        // From the first corner, so large coordinates don't cost precision
        float twice = 0.0f;
        for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
        {
            twice += Side(polygon[0], polygon[i], polygon[i + 1]);
        }
        return 0.5f * twice;
    }

    ConvexPolygon IntersectConvex(const ConvexPolygon &subject, const ConvexPolygon &clip)
    {
        // A clip without area keeps nothing: every point would count as inside each of its edges
        if (subject.size() < 3 || clip.size() < 3 || !(PolygonArea(clip) > 0.0f))
        {
            return {};
        }
        ConvexPolygon result = subject;
        ConvexPolygon input;
        for (std::size_t edge = 0; edge < clip.size() && !result.empty(); ++edge)
        {
            const Math::Vector2 &from = clip[edge];
            const Math::Vector2 &to = clip[(edge + 1) % clip.size()];
            input.swap(result);
            result.clear();
            for (std::size_t i = 0; i < input.size(); ++i)
            {
                const Math::Vector2 &current = input[i];
                const Math::Vector2 &next = input[(i + 1) % input.size()];
                const float currentSide = Side(from, to, current);
                const float nextSide = Side(from, to, next);
                if (currentSide >= 0.0f)
                {
                    result.push_back(current);
                }
                if ((currentSide > 0.0f && nextSide < 0.0f) || (currentSide < 0.0f && nextSide > 0.0f))
                {
                    const float t = currentSide / (currentSide - nextSide);
                    result.push_back(Math::Vector2{current.x + (next.x - current.x) * t,
                                                   current.y + (next.y - current.y) * t});
                }
            }
        }
        if (!(PolygonArea(result) > MinArea))
        {
            result.clear();
        }
        return result;
    }

    bool ConvexContains(const ConvexPolygon &polygon, const Math::Vector2 &point)
    {
        if (polygon.size() < 3)
        {
            return false;
        }
        for (std::size_t i = 0; i < polygon.size(); ++i)
        {
            const Math::Vector2 &from = polygon[i];
            const Math::Vector2 &to = polygon[(i + 1) % polygon.size()];
            // A thousandth of a canvas unit outside an edge still counts, so a point on a turned edge is not lost to rounding
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (Side(from, to, point) < -1e-3f * length)
            {
                return false;
            }
        }
        return true;
    }

    Rect BoundsOf(const ConvexPolygon &polygon)
    {
        if (polygon.empty())
        {
            return Rect{};
        }
        float minX = polygon.front().x;
        float minY = polygon.front().y;
        float maxX = minX;
        float maxY = minY;
        for (const Math::Vector2 &point : polygon)
        {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        return Rect{minX, minY, maxX - minX, maxY - minY};
    }

    std::optional<Rect> AsAxisAlignedRect(const ConvexPolygon &polygon, const float tolerance)
    {
        if (polygon.size() < 4)
        {
            return std::nullopt;
        }
        const Rect bounds = BoundsOf(polygon);
        // Every corner on a vertical side and a horizontal side of the bounds (more than four corners are points along them)
        for (const Math::Vector2 &point : polygon)
        {
            const bool onVertical = std::abs(point.x - bounds.XMin()) <= tolerance ||
                                    std::abs(point.x - bounds.XMax()) <= tolerance;
            const bool onHorizontal = std::abs(point.y - bounds.YMin()) <= tolerance ||
                                      std::abs(point.y - bounds.YMax()) <= tolerance;
            if (!onVertical || !onHorizontal)
            {
                return std::nullopt;
            }
        }
        // And the area of the whole box, so a triangle on three of its corners is not taken for it
        if (std::abs(PolygonArea(polygon) - bounds.width * bounds.height) >
            tolerance * (bounds.width + bounds.height) + tolerance)
        {
            return std::nullopt;
        }
        return bounds;
    }
}
