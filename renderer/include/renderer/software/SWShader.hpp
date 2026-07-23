#pragma once

#include <unordered_map>
#include <string>
#include <array>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/common/IShader.hpp"

namespace Renderer::Software
{
    enum class SWShaderType { Unlit, Lit };

    class SWShader : public Common::IShader
    {
    public:
        explicit SWShader(SWShaderType type = SWShaderType::Unlit) : m_type(type) {}

        [[nodiscard]] SWShaderType GetType() const { return m_type; }

        bool LoadFromStrings(const std::string &, const std::string &) override { return true; }
        void Bind() const override {}
        void Unbind() const override {}
        bool IsValid() const override { return true; }

        void SetFloat(const std::string &n, float v) override { m_floats[n] = v; }
        void SetInt(const std::string &n, int v) override { m_ints[n] = v; }
        void SetBool(const std::string &n, bool v) override { m_ints[n] = v ? 1 : 0; }
        void SetVec2(const std::string &n, const N2Engine::Math::Vector2 &v) override { m_vec4s[n] = {v.x, v.y, 0, 0}; }
        void SetVec3(const std::string &n, const N2Engine::Math::Vector3 &v) override { m_vec4s[n] = {v.x, v.y, v.z, 0}; }
        void SetVec4(const std::string &n, const N2Engine::Math::Vector4 &v) override { m_vec4s[n] = {v.x, v.y, v.z, v.w}; }

        void SetMat4(const std::string &n, const N2Engine::Math::Matrix<float, 4, 4> &v) override
        {
            /* unused in SW */
        }

        void SetVec2(const std::string &n, float x, float y) override { m_vec4s[n] = {x, y, 0, 0}; }
        void SetVec3(const std::string &n, float x, float y, float z) override { m_vec4s[n] = {x, y, z, 0}; }
        void SetVec4(const std::string &n, float x, float y, float z, float w) override { m_vec4s[n] = {x, y, z, w}; }

        [[nodiscard]] float GetFloat(const std::string &n, float def = 0.f) const
        {
            auto it = m_floats.find(n);
            return it != m_floats.end() ? it->second : def;
        }

    private:
        SWShaderType m_type;
        std::unordered_map<std::string, float> m_floats;
        std::unordered_map<std::string, int> m_ints;
        std::unordered_map<std::string, std::array<float, 4>> m_vec4s;
    };
}
