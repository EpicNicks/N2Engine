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
        Math::Vector3 _position{0.0f, 0.0f, 0.0f};
        Math::Quaternion _rotation = Math::Quaternion::Identity();
        Math::Vector3 _scale{1.0f, 1.0f, 1.0f};

        mutable Matrix4 _cachedMatrix;
        mutable bool _matrixDirty = true;

        void BuildMatrix() const;

        const Matrix4 &GetMatrix() const;

        void SetPosition(Math::Vector3 position);
        void SetRotation(Math::Quaternion rotation);
        void SetScale(Math::Vector3 scale);
        void SetPositionAndRotation(Math::Vector3 position, Math::Quaternion rotation);

    public:
        Math::Vector3 GetPosition() const;
        Math::Quaternion GetRotation() const;
        Math::Vector3 GetScale() const;

        static Transform Identity();
        static Transform Combine(const Transform &parent, const Transform &child);

        bool operator==(const Transform &other) const;
        bool operator!=(const Transform &other) const;
    };
}