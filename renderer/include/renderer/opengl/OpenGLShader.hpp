#pragma once

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
        /// uStageDim (RenderState::dim), declared by the built-in fragment shaders
        GLint stageDimLoc = -1;
        /// uTintSaturation, uTintInside and uTintCircle (RenderState::tint), declared by the built-in fragment shaders
        GLint tintSaturationLoc = -1;
        GLint tintInsideLoc = -1;
        GLint tintCircleLoc = -1;
        /// uFramebufferEncodes: declared by the built-in lit shader, which tints in sRGB when GL_FRAMEBUFFER_SRGB is on
        GLint framebufferEncodesLoc = -1;
        /// n2_TargetHeight: declared by the OpenGL prelude of the user shaders (n2_FragCoord() flips gl_FragCoord with it)
        GLint targetHeightLoc = -1;
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
        /// A user shader's mat3 (`dimension` 3, 9 values) or mat4 (16) uniform from row-major values (uploaded transposed, as SetMat4 does)
        void SetMatrixRowMajor(const std::string &name, const float *values, unsigned dimension);

        // convenience methods
        void SetVec2(const std::string &name, float x, float y);
        void SetVec3(const std::string &name, float x, float y, float z);
        void SetVec4(const std::string &name, float x, float y, float z, float w);

        GLuint GetId() const;
        /// A user shader's program (stage 4): a uniform the compiler left out (it is unused) is not worth a warning, since a
        /// shader declares what it may not read. The built-in programs warn about a missing uniform once, as always
        void SetQuiet(const bool quiet) { _quiet = quiet; }
        inline const ShaderUniforms &GetCommonUniforms() const { return _commonUniforms; }

    private:
        GLuint _programId;
        bool _quiet = false;
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