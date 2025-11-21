#pragma once

#include <cstdint>
#include <string>

namespace N2Engine::Math
{
    struct Vector2;
    struct Vector3;
    struct Vector4;
    template <typename T, size_t Rows, size_t Cols>
    class Matrix;
}

namespace Renderer::Common
{
    class IMaterial;

    class IShader
    {
    public:
        virtual ~IShader() = default;

        /// @brief Loads and compiles shader from source strings
        /// @warning For proper resource management, prefer using
        ///          IRenderer::CreateShaderProgram() instead of calling this directly
        virtual bool LoadFromStrings(const std::string &vertexSource, const std::string &fragmentSource) = 0;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual bool IsValid() const = 0;

        virtual void SetFloat(const std::string &name, float value) = 0;
        virtual void SetInt(const std::string &name, int value) = 0;
        virtual void SetBool(const std::string &name, bool value) = 0;
        virtual void SetVec2(const std::string &name, const N2Engine::Math::Vector2 &value) = 0;
        virtual void SetVec3(const std::string &name, const N2Engine::Math::Vector3 &value) = 0;
        virtual void SetVec4(const std::string &name, const N2Engine::Math::Vector4 &value) = 0;
        virtual void SetMat4(const std::string &name, const N2Engine::Math::Matrix<float, 4, 4> &value) = 0;

        // convenience methods
        virtual void SetVec2(const std::string &name, float x, float y) = 0;
        virtual void SetVec3(const std::string &name, float x, float y, float z) = 0;
        virtual void SetVec4(const std::string &name, float x, float y, float z, float w) = 0;
    };
}