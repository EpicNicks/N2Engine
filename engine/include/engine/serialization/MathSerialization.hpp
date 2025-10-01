#pragma once

#include <nlohmann/json.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <math/Quaternion.hpp>
#include "engine/common/Color.hpp"

// Math type serialization
namespace N2Engine::Math
{
    inline void to_json(nlohmann::json &j, const Vector3 &v)
    {
        j = nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
    }

    inline void from_json(const nlohmann::json &j, Vector3 &v)
    {
        j.at("x").get_to(v.x);
        j.at("y").get_to(v.y);
        j.at("z").get_to(v.z);
        v.w = 0.0f;
    }

    inline void to_json(nlohmann::json &j, const Quaternion &q)
    {
        j = nlohmann::json{{"w", q.GetW()}, {"x", q.GetX()}, {"y", q.GetY()}, {"z", q.GetZ()}};
    }

    inline void from_json(const nlohmann::json &j, Quaternion &q)
    {
        q = Quaternion(j["w"], j["x"], j["y"], j["z"]);
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
        c.r = j["r"];
        c.g = j["g"];
        c.b = j["b"];
        c.a = j["a"];
    }
}