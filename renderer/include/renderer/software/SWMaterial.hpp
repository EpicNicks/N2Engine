#pragma once

#include <unordered_map>
#include <array>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/common/IMaterial.hpp"

namespace Renderer::Software
{
    class SWMaterial : public Common::IMaterial
    {
    public:
        explicit SWMaterial(Common::IShader *shader, Common::ITexture *texture = nullptr)
            : m_shader(shader), m_texture(texture) {}

        void SetInt(const std::string &n, int v) override { m_ints[n] = v; }
        void SetFloat(const std::string &n, float v) override { m_floats[n] = v; }
        void SetVec2(const std::string &n, float x, float y) override { m_vec4s[n] = {x, y, 0, 0}; }
        void SetVec2(const std::string &n, N2Engine::Math::Vector2 &v) override { m_vec4s[n] = {v.x, v.y, 0, 0}; }
        void SetVec3(const std::string &n, float x, float y, float z) override { m_vec4s[n] = {x, y, z, 0}; }
        void SetVec3(const std::string &n, N2Engine::Math::Vector3 &v) override { m_vec4s[n] = {v.x, v.y, v.z, 0}; }
        void SetVec4(const std::string &n, float x, float y, float z, float w) override { m_vec4s[n] = {x, y, z, w}; }
        void SetVec4(const std::string &n, N2Engine::Math::Vector4 &v) override { m_vec4s[n] = {v.x, v.y, v.z, v.w}; }
        void SetColor(const std::string &n, float r, float g, float b, float a) override { m_vec4s[n] = {r, g, b, a}; }
        void SetTexture(Common::ITexture *t) override { m_texture = t; }

        [[nodiscard]] Common::IShader* GetShader() const override { return m_shader; }
        [[nodiscard]] Common::ITexture* GetTexture() const override { return m_texture; }
        [[nodiscard]] bool IsValid() const override { return m_shader != nullptr; }

        [[nodiscard]] std::array<float, 4> GetVec4(const std::string &n, std::array<float, 4> def = {1, 1, 1, 1}) const
        {
            const auto it = m_vec4s.find(n);
            return it != m_vec4s.end() ? it->second : def;
        }

        [[nodiscard]] float GetFloat(const std::string &n, float def = 0.f) const
        {
            const auto it = m_floats.find(n);
            return it != m_floats.end() ? it->second : def;
        }

    private:
        Common::IShader *m_shader = nullptr;
        Common::ITexture *m_texture = nullptr;
        std::unordered_map<std::string, int> m_ints;
        std::unordered_map<std::string, float> m_floats;
        std::unordered_map<std::string, std::array<float, 4>> m_vec4s;
    };
}
