#pragma once

#include <nlohmann/json.hpp>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <math/Quaternion.hpp>
#include "engine/common/Color.hpp"

// serialization methods need to be in the same namespace as their types
namespace N2Engine::Math
{
    inline void to_json(nlohmann::json &j, const Vector3 &v)
    {
        j = nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
    }

    inline void from_json(const nlohmann::json &j, Vector3 &v)
    {
        v.x = j.value("x", 0.0f);
        v.y = j.value("y", 0.0f);
        v.z = j.value("z", 0.0f);
        v.w = 0.0f;
    }

    inline void to_json(nlohmann::json &j, const Vector4 &v)
    {
        j = nlohmann::json{{"w", v.w}, {"x", v.x}, {"y", v.y}, {"z", v.z}};
    }

    inline void from_json(const nlohmann::json &j, Vector4 &v)
    {
        v.w = j.value("w", 0.0f);
        v.x = j.value("x", 0.0f);
        v.y = j.value("y", 0.0f);
        v.z = j.value("z", 0.0f);
    }

    inline void to_json(nlohmann::json &j, const Quaternion &q)
    {
        j = nlohmann::json{{"w", q.GetW()}, {"x", q.GetX()}, {"y", q.GetY()}, {"z", q.GetZ()}};
    }

    inline void from_json(const nlohmann::json &j, Quaternion &q)
    {
        float w = j.value("w", 0.0f);
        float x = j.value("x", 0.0f);
        float y = j.value("y", 0.0f);
        float z = j.value("z", 0.0f);
        q = {w, x, y, z};
        if (!q.IsNormalized())
        {
            if (q.LengthSquared() < 1e-6f)
            {
                q = Quaternion::Identity();
            }
            else
            {
                q.Normalize();
            }
        }
    }
}

namespace N2Engine::Common
{
    inline void to_json(nlohmann::json &j, const Color &c)
    {
        j = nlohmann::json{{"r", c.r}, {"g", c.g}, {"b", c.b}, {"a", c.a}};
    }

    inline void from_json(const nlohmann::json &j, Color &c)
    {
        c.r = j.value("r", 1.0f);  // White opaque
        c.g = j.value("g", 1.0f);
        c.b = j.value("b", 1.0f);
        c.a = j.value("a", 1.0f);
    }
}