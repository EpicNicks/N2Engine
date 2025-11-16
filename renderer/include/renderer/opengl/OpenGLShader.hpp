#pragma once

#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <unordered_map>

#include "renderer/common/IShader.hpp"

namespace Renderer::OpenGL
{
    struct ShaderUniforms
    {
        GLint modelLoc = -1;
        GLint viewLoc = -1;
        GLint projectionLoc = -1;
        GLint textureLoc = -1;
        GLint colorLoc = -1;
    };

    class OpenGLShader : public Common::IShader
    {
    public:
        OpenGLShader();
        virtual ~OpenGLShader();

        bool LoadFromStrings(const std::string &vertexSource, const std::string &fragmentSource) override;

        void Bind() const override;
        void Unbind() const override;

        bool IsValid() const override;

        void SetFloat(const std::string &name, float value) override;
        void SetInt(const std::string &name, int value) override;
        void SetBool(const std::string &name, bool value) override;

        void SetVec2(const std::string &name, const N2Engine::Math::Vector2 &value) override;
        void SetVec3(const std::string &name, const N2Engine::Math::Vector3 &value) override;
        void SetVec4(const std::string &name, const N2Engine::Math::Vector4 &value) override;

        void SetMat4(const std::string &name, const N2Engine::Math::Matrix<float, 4, 4> &value) override;

        // Convenience overloads
        void SetVec2(const std::string &name, float x, float y) override;
        void SetVec3(const std::string &name, float x, float y, float z) override;
        void SetVec4(const std::string &name, float x, float y, float z, float w) override;

        GLuint GetId() const;
        inline const ShaderUniforms &GetCommonUniforms() const { return _commonUniforms; }

    private:
        GLuint _programId;
        mutable std::unordered_map<std::string, int> _uniformLocationCache;
        ShaderUniforms _commonUniforms;

        // Helper methods
        GLuint CompileShader(const std::string &source, GLenum shaderType);
        bool LinkProgram(GLuint vertexShader, GLuint fragmentShader);

        GLint GetUniformLocation(const std::string &name) const;
        void CacheCommonUniforms();

        void CheckCompileErrors(unsigned int shader, const std::string &type);
        void CheckLinkErrors(unsigned int program);
    };
}