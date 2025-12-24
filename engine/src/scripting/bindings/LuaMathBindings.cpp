#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <math/Quaternion.hpp>
#include <math/Matrix.hpp>
#include <math/Constants.hpp>
#include <math/Random.hpp>

namespace N2Engine::Scripting::Bindings
{
    void BindMath(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        lua["Math"] = lua.create_table_with(
            "PI", Math::Constants::PI_F,
            "TWO_PI", Math::Constants::TWO_PI,
            "HALF_PI", Math::Constants::HALF_PI,
            "DEG_TO_RAD", Math::Constants::DEG_TO_RAD,
            "RAD_TO_DEG", Math::Constants::RAD_TO_DEG,
            "EPSILON", Math::Constants::EPSILON
        );

        // ===== Random =====
        lua["Random"] = lua.create_table_with(
            "Float", &Math::Random::RandomFloat,
            "Range", sol::overload(
                &Math::Random::RandomRange,
                &Math::Random::RandomInt
            ),
            "Int", &Math::Random::RandomInt,
            "InUnitCircle", &Math::Random::RandomInUnitCircle,
            "OnUnitCircle", &Math::Random::RandomOnUnitCircle
        );

        lua.new_usertype<Math::Vector2>(
            "Vector2",
            sol::call_constructor,
            sol::constructors<
                Math::Vector2(),
                Math::Vector2(float, float),
                Math::Vector2(float)
            >(),

            // Fields
            "x", &Math::Vector2::x,
            "y", &Math::Vector2::y,

            // Static constants
            "Zero", sol::var(Math::Vector2::Zero),
            "One", sol::var(Math::Vector2::One),
            "Up", sol::var(Math::Vector2::Up),
            "Down", sol::var(Math::Vector2::Down),
            "Left", sol::var(Math::Vector2::Left),
            "Right", sol::var(Math::Vector2::Right),

            // Member methods
            "Dot", [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.Dot(other); },
            "Cross", [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.Cross(other); },
            "Length", [](const Math::Vector2 &self) { return self.Length(); },
            "LengthSquared", [](const Math::Vector2 &self) { return self.LengthSquared(); },
            "Normalized", [](const Math::Vector2 &self) { return self.Normalized(); },
            "Normalize", [](Math::Vector2 &self) { return self.Normalize(); },
            "Distance", [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.Distance(other); },
            "DistanceSquared", [](const Math::Vector2 &self, const Math::Vector2 &other)
            {
                return self.DistanceSquared(other);
            },
            "Angle", [](const Math::Vector2 &self) { return self.Angle(); },
            "AngleTo", [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.AngleTo(other); },
            "Perpendicular", [](const Math::Vector2 &self) { return self.Perpendicular(); },
            "PerpendicularCW", [](const Math::Vector2 &self) { return self.PerpendicularCW(); },
            "Rotated", [](const Math::Vector2 &self, float radians) { return self.Rotated(radians); },
            "Rotate", [](Math::Vector2 &self, float radians) { return self.Rotate(radians); },
            "Project", [](const Math::Vector2 &self, const Math::Vector2 &onto) { return self.Project(onto); },
            "Reject", [](const Math::Vector2 &self, const Math::Vector2 &onto) { return self.Reject(onto); },
            "Reflect", [](const Math::Vector2 &self, const Math::Vector2 &normal) { return self.Reflect(normal); },
            "Scale", [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.Scale(other); },
            "Sign", [](const Math::Vector2 &self) { return self.Sign(); },
            "IsZero", [](const Math::Vector2 &self, float tolerance) { return self.IsZero(tolerance); },
            "IsNormalized", [](const Math::Vector2 &self, float tolerance) { return self.IsNormalized(tolerance); },
            "IsParallel", [](const Math::Vector2 &self, const Math::Vector2 &other, float tolerance)
            {
                return self.IsParallel(other, tolerance);
            },
            "IsPerpendicular", [](const Math::Vector2 &self, const Math::Vector2 &other, float tolerance)
            {
                return self.IsPerpendicular(other, tolerance);
            },

            // Member methods with overloads
            "Lerp", [](const Math::Vector2 &self, const Math::Vector2 &other, float t) { return self.Lerp(other, t); },
            "Slerp", [](const Math::Vector2 &self, const Math::Vector2 &other, float t)
            {
                return self.Slerp(other, t);
            },
            "MoveTowards", [](const Math::Vector2 &self, const Math::Vector2 &target, float maxDelta)
            {
                return self.MoveTowards(target, maxDelta);
            },
            "ClampMagnitude", [](const Math::Vector2 &self, float maxLength) { return self.ClampMagnitude(maxLength); },

            // Static methods with overloads
            "FromAngle", sol::overload(
                [](float radians) { return Math::Vector2::FromAngle(radians); },
                [](float radians, float magnitude) { return Math::Vector2::FromAngle(radians, magnitude); }
            ),

            // Static helpers
            "Min", sol::overload(
                [](const Math::Vector2 &a, const Math::Vector2 &b) { return Math::Vector2::Min(a, b); },
                [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.Min(other); }
            ),
            "Max", sol::overload(
                [](const Math::Vector2 &a, const Math::Vector2 &b) { return Math::Vector2::Max(a, b); },
                [](const Math::Vector2 &self, const Math::Vector2 &other) { return self.Max(other); }
            ),
            "Clamp", [](const Math::Vector2 &self, const Math::Vector2 &min, const Math::Vector2 &max)
            {
                return self.Clamp(min, max);
            },
            "Floor", [](const Math::Vector2 &self) { return self.Floor(); },
            "Ceil", [](const Math::Vector2 &self) { return self.Ceil(); },
            "Round", [](const Math::Vector2 &self) { return self.Round(); },
            "Abs", [](const Math::Vector2 &self) { return self.Abs(); },

            // Operators
            sol::meta_function::addition, [](const Math::Vector2 &a, const Math::Vector2 &b) { return a + b; },
            sol::meta_function::subtraction, [](const Math::Vector2 &a, const Math::Vector2 &b) { return a - b; },
            sol::meta_function::unary_minus, [](const Math::Vector2 &v) { return -v; },
            sol::meta_function::multiplication, sol::overload(
                [](const Math::Vector2 &v, float s) { return v * s; },
                [](float s, const Math::Vector2 &v) { return v * s; }
            ),
            sol::meta_function::division, [](const Math::Vector2 &v, float s) { return v / s; },
            sol::meta_function::equal_to, [](const Math::Vector2 &a, const Math::Vector2 &b) { return a == b; }
        );


        lua.new_usertype<Math::Vector3>(
            "Vector3",
            sol::call_constructor,
            sol::constructors<
                Math::Vector3(),
                Math::Vector3(float, float, float),
                Math::Vector3(float)
            >(),

            "x", &Math::Vector3::x,
            "y", &Math::Vector3::y,
            "z", &Math::Vector3::z,

            "Zero", sol::var(Math::Vector3::Zero),
            "One", sol::var(Math::Vector3::One),
            "Up", sol::var(Math::Vector3::Up),
            "Down", sol::var(Math::Vector3::Down),
            "Left", sol::var(Math::Vector3::Left),
            "Right", sol::var(Math::Vector3::Right),
            "Forward", sol::var(Math::Vector3::Forward),
            "Back", sol::var(Math::Vector3::Back),

            "Dot", &Math::Vector3::Dot,
            "Cross", &Math::Vector3::Cross,
            "Length", &Math::Vector3::Length,
            "LengthSquared", &Math::Vector3::LengthSquared,
            "Normalized", &Math::Vector3::Normalized,
            "Normalize", &Math::Vector3::Normalize,
            "Distance", &Math::Vector3::Distance,
            "DistanceSquared", &Math::Vector3::DistanceSquared,
            "AngleTo", &Math::Vector3::AngleTo,
            "Project", &Math::Vector3::Project,
            "ProjectOnPlane", &Math::Vector3::ProjectOnPlane,
            "Reject", &Math::Vector3::Reject,
            "Reflect", &Math::Vector3::Reflect,
            "Scale", &Math::Vector3::Scale,
            "Lerp", sol::resolve<Math::Vector3(const Math::Vector3 &, float) const>(&Math::Vector3::Lerp),
            "Slerp", sol::resolve<Math::Vector3(const Math::Vector3 &, float) const>(&Math::Vector3::Slerp),
            "MoveTowards", sol::resolve<Math::Vector3(const Math::Vector3 &, float) const>(&Math::Vector3::MoveTowards),
            "ClampMagnitude", sol::resolve<Math::Vector3(float) const>(&Math::Vector3::ClampMagnitude),
            "GetOrthogonal", &Math::Vector3::GetOrthogonal,
            "Min", sol::resolve<Math::Vector3(const Math::Vector3 &) const>(&Math::Vector3::Min),
            "Max", sol::resolve<Math::Vector3(const Math::Vector3 &) const>(&Math::Vector3::Max),
            "Clamp",
            sol::resolve<Math::Vector3(const Math::Vector3 &, const Math::Vector3 &) const>(&Math::Vector3::Clamp),
            "Floor", sol::resolve<Math::Vector3() const>(&Math::Vector3::Floor),
            "Ceil", sol::resolve<Math::Vector3() const>(&Math::Vector3::Ceil),
            "Round", sol::resolve<Math::Vector3() const>(&Math::Vector3::Round),
            "Abs", sol::resolve<Math::Vector3() const>(&Math::Vector3::Abs),
            "Sign", &Math::Vector3::Sign,
            "IsZero", &Math::Vector3::IsZero,
            "IsNormalized", &Math::Vector3::IsNormalized,
            "IsParallel", &Math::Vector3::IsParallel,
            "IsPerpendicular", &Math::Vector3::IsPerpendicular,

            sol::meta_function::addition,
            [](const Math::Vector3 &a, const Math::Vector3 &b) { return a + b; },

            sol::meta_function::subtraction,
            [](const Math::Vector3 &a, const Math::Vector3 &b) { return a - b; },

            sol::meta_function::unary_minus,
            [](const Math::Vector3 &v) { return -v; },

            sol::meta_function::multiplication, sol::overload(
                [](const Math::Vector3 &v, float s) { return v * s; },
                [](float s, const Math::Vector3 &v) { return v * s; }
            ),

            sol::meta_function::division,
            [](const Math::Vector3 &v, float s) { return v / s; },

            sol::meta_function::equal_to,
            [](const Math::Vector3 &a, const Math::Vector3 &b) { return a == b; }
        );

        lua.new_usertype<Math::Vector4>(
            "Vector4",
            sol::call_constructor,
            sol::constructors<
                Math::Vector4(),
                Math::Vector4(float, float, float, float),
                Math::Vector4(float)
            >(),

            "w", &Math::Vector4::w,
            "x", &Math::Vector4::x,
            "y", &Math::Vector4::y,
            "z", &Math::Vector4::z,

            "Zero", sol::var(Math::Vector4::Zero),
            "One", sol::var(Math::Vector4::One),

            "Dot", &Math::Vector4::Dot,
            "Length", &Math::Vector4::Length,
            "LengthSquared", &Math::Vector4::LengthSquared,
            "Normalized", &Math::Vector4::Normalized,
            "Normalize", &Math::Vector4::Normalize,
            "Distance", &Math::Vector4::Distance,
            "DistanceSquared", &Math::Vector4::DistanceSquared,

            sol::meta_function::addition,
            [](const Math::Vector4 &a, const Math::Vector4 &b) { return a + b; },

            sol::meta_function::subtraction,
            [](const Math::Vector4 &a, const Math::Vector4 &b) { return a - b; },

            sol::meta_function::multiplication, sol::overload(
                [](const Math::Vector4 &v, float s) { return v * s; },
                [](float s, const Math::Vector4 &v) { return v * s; }
            ),

            sol::meta_function::division,
            [](const Math::Vector4 &v, float s) { return v / s; }
        );

        // ===== Quaternion =====
        lua.new_usertype<Math::Quaternion>(
            "Quaternion",
            sol::call_constructor,
            sol::constructors<
                Math::Quaternion(),
                Math::Quaternion(float, float, float, float)
            >(),

            "w", sol::property(&Math::Quaternion::GetW),
            "x", sol::property(&Math::Quaternion::GetX),
            "y", sol::property(&Math::Quaternion::GetY),
            "z", sol::property(&Math::Quaternion::GetZ),

            "Identity", sol::var(Math::Quaternion::Identity),

            "FromAxisAngle", &Math::Quaternion::FromAxisAngle,
            "FromEulerAngles", sol::overload(
                static_cast<Math::Quaternion(*)(float, float, float)>(&Math::Quaternion::FromEulerAngles),
                static_cast<Math::Quaternion(*)(const Math::Vector3 &)>(&Math::Quaternion::FromEulerAngles)
            ),
            "LookRotation", &Math::Quaternion::LookRotation,
            "Slerp", &Math::Quaternion::Slerp,
            "Lerp", &Math::Quaternion::Lerp,

            "Length", &Math::Quaternion::Length,
            "LengthSquared", &Math::Quaternion::LengthSquared,
            "Normalized", &Math::Quaternion::Normalized,
            "Normalize", &Math::Quaternion::Normalize,
            "Conjugate", &Math::Quaternion::Conjugate,
            "Inverse", &Math::Quaternion::Inverse,
            "Dot", &Math::Quaternion::Dot,
            "Angle", &Math::Quaternion::Angle,
            "Rotate", &Math::Quaternion::Rotate,
            "ToEulerAngles", &Math::Quaternion::ToEulerAngles,
            "ToMatrix", &Math::Quaternion::ToMatrix,
            "IsNormalized", &Math::Quaternion::IsNormalized,
            "IsIdentity", &Math::Quaternion::IsIdentity,

            sol::meta_function::addition, &Math::Quaternion::operator+,
            sol::meta_function::subtraction, &Math::Quaternion::operator-,
            sol::meta_function::multiplication, sol::overload(
                static_cast<Math::Quaternion(Math::Quaternion::*)(const Math::Quaternion &) const>(&
                    Math::Quaternion::operator*),
                static_cast<Math::Quaternion(Math::Quaternion::*)(float) const>(&Math::Quaternion::operator*),
                static_cast<Math::Vector3(Math::Quaternion::*)(const Math::Vector3 &) const>(&Math::Quaternion::operator
                    *)
            ),
            sol::meta_function::division, &Math::Quaternion::operator/
        );

        // ===== Matrix4x4 =====
        lua.new_usertype<Math::Matrix<float, 4, 4>>(
            "Matrix4",
            sol::no_constructor,

            "Identity", &Math::Matrix<float, 4, 4>::identity,
            "Translation", &Math::Matrix<float, 4, 4>::Translation,
            "Scale", &Math::Matrix<float, 4, 4>::Scale,
            "RotationX", &Math::Matrix<float, 4, 4>::RotationX,
            "RotationY", &Math::Matrix<float, 4, 4>::RotationY,
            "RotationZ", &Math::Matrix<float, 4, 4>::RotationZ,

            "TransformPoint", &Math::Matrix<float, 4, 4>::TransformPoint,
            "transpose", &Math::Matrix<float, 4, 4>::transpose,
            "inverse", &Math::Matrix<float, 4, 4>::inverse,
            "determinant", &Math::Matrix<float, 4, 4>::determinant,

            sol::meta_function::addition, &Math::Matrix<float, 4, 4>::operator+,
            sol::meta_function::subtraction,
            &Math::Matrix<float, 4, 4>::operator-,
            sol::meta_function::multiplication, sol::overload(
                static_cast<Math::Matrix<float, 4, 4>(Math::Matrix<
                    float, 4, 4>::*)(const Math::Matrix<float, 4, 4> &) const>(&
                    Math::Matrix<float, 4, 4>::operator*),
                static_cast<Math::Matrix<float, 4, 4>(Math::Matrix<
                    float, 4, 4>::*)(float) const>(&Math::Matrix<
                    float, 4, 4>::operator*)
            )
        );
    }
}
