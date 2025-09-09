#pragma once

#include <math/Matrix.hpp>
#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>

#include "engine/base/EventHandler.hpp"

namespace N2Engine
{
    class Transform
    {
        friend class Positionable;
        using Matrix4 = Math::Matrix<float, 4, 4>;

    private:
        Matrix4 _transform;

        void SetPosition(Math::Vector3 position);
        void SetRotation(Math::Quaternion rotation);
        void SetScale(Math::Vector3 scale);
        void SetPositionAndRotation(Math::Vector3 position, Math::Quaternion rotation);

    public:
        Math::Vector3 GetPosition() const;
        Math::Quaternion GetRotation() const;
        Math::Vector3 GetScale() const;

        static constexpr Transform Identity();
        static Transform Combine(const Transform &parent, const Transform &child);

        bool Transform::operator==(const Transform &other) const;
        bool Transform::operator!=(const Transform &other) const;
    };
}