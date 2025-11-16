#include <memory>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/opengl/OpenGLShader.hpp"

using namespace Renderer::OpenGL;

OpenGLShader::OpenGLShader()
    : _programId{0}
{
}

OpenGLShader::~OpenGLShader()
{
    if (_programId != 0)
    {
        glDeleteProgram(_programId);
        _programId = 0;
    }
}

GLuint OpenGLShader::GetId() const
{
    return _programId;
}

bool OpenGLShader::LoadFromStrings(const std::string &vertexSource, const std::string &fragmentSource)
{
    if (_programId != 0)
    {
        glDeleteProgram(_programId);
        _programId = 0;
        _uniformLocationCache.clear();
    }

    unsigned int vertexShader = CompileShader(vertexSource, GL_VERTEX_SHADER);
    if (vertexShader == 0)
        return false;

    unsigned int fragmentShader = CompileShader(fragmentSource, GL_FRAGMENT_SHADER);
    if (fragmentShader == 0)
    {
        glDeleteShader(vertexShader);
        return false;
    }

    bool success = LinkProgram(vertexShader, fragmentShader);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (success)
    {
        CacheCommonUniforms(); // Add this line
    }

    return success;
}

void OpenGLShader::Bind() const
{
    if (_programId != 0)
    {
        glUseProgram(_programId);
    }
}

void OpenGLShader::Unbind() const
{
    glUseProgram(0);
}

bool OpenGLShader::IsValid() const
{
    return _programId != 0;
}

void OpenGLShader::SetFloat(const std::string &name, float value)
{
    glUniform1f(GetUniformLocation(name), value);
}

void OpenGLShader::SetInt(const std::string &name, int value)
{
    glUniform1i(GetUniformLocation(name), value);
}

void OpenGLShader::SetBool(const std::string &name, bool value)
{
    glUniform1i(GetUniformLocation(name), value ? 1 : 0);
}

void OpenGLShader::SetVec2(const std::string &name, const N2Engine::Math::Vector2 &value)
{
    glUniform2f(GetUniformLocation(name), value.x, value.y);
}

void OpenGLShader::SetVec3(const std::string &name, const N2Engine::Math::Vector3 &value)
{
    glUniform3f(GetUniformLocation(name), value.x, value.y, value.z);
}

void OpenGLShader::SetVec4(const std::string &name, const N2Engine::Math::Vector4 &value)
{
    glUniform4f(GetUniformLocation(name), value.x, value.y, value.z, value.w);
}

void OpenGLShader::SetMat4(const std::string &name, const N2Engine::Math::Matrix<float, 4, 4> &value)
{
    // Assuming your Matrix4 has a method to get the raw float data
    // If not, you'll need to add one or adjust this
    glUniformMatrix4fv(GetUniformLocation(name), 1, GL_TRUE, value.Data());
}

void OpenGLShader::SetVec2(const std::string &name, float x, float y)
{
    glUniform2f(GetUniformLocation(name), x, y);
}

void OpenGLShader::SetVec3(const std::string &name, float x, float y, float z)
{
    glUniform3f(GetUniformLocation(name), x, y, z);
}

void OpenGLShader::SetVec4(const std::string &name, float x, float y, float z, float w)
{
    glUniform4f(GetUniformLocation(name), x, y, z, w);
}

GLuint OpenGLShader::CompileShader(const std::string &source, GLenum shaderType)
{
    unsigned int shader = glCreateShader(shaderType);
    const char *src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    // Check for compilation errors
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        CheckCompileErrors(shader, shaderType == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT");
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool OpenGLShader::LinkProgram(GLuint vertexShader, GLuint fragmentShader)
{
    _programId = glCreateProgram();
    glAttachShader(_programId, vertexShader);
    glAttachShader(_programId, fragmentShader);
    glLinkProgram(_programId);

    // Check for linking errors
    int success;
    glGetProgramiv(_programId, GL_LINK_STATUS, &success);
    if (!success)
    {
        CheckLinkErrors(_programId);
        glDeleteProgram(_programId);
        _programId = 0;
        return false;
    }

    return true;
}

GLint OpenGLShader::GetUniformLocation(const std::string &name) const
{
    // Check cache first
    auto it = _uniformLocationCache.find(name);
    if (it != _uniformLocationCache.end())
    {
        return it->second;
    }

    // Query OpenGL for location
    GLint location = glGetUniformLocation(_programId, name.c_str());

    // Cache the location (even if it's -1, to avoid repeated queries)
    _uniformLocationCache[name] = location;

    // Warn if uniform not found (but only once)
    if (location == -1)
    {
        std::cerr << "Warning: Uniform '" << name << "' not found in shader!" << std::endl;
    }

    return location;
}

void OpenGLShader::CheckCompileErrors(unsigned int shader, const std::string &type)
{
    int infoLogLength;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0)
    {
        std::unique_ptr<char[]> infoLog = std::make_unique<char[]>(infoLogLength);
        glGetShaderInfoLog(shader, infoLogLength, nullptr, infoLog.get());

        std::cerr << "Shader compilation error (" << type << "):\n"
                  << infoLog << std::endl;
    }
}

void OpenGLShader::CheckLinkErrors(unsigned int program)
{
    int infoLogLength;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLength);

    if (infoLogLength > 0)
    {
        std::unique_ptr<char[]> infoLog = std::make_unique<char[]>(infoLogLength);
        glGetProgramInfoLog(program, infoLogLength, nullptr, infoLog.get());
        std::cerr << "Shader linking error:\n"
                  << infoLog << std::endl;
    }
}

void OpenGLShader::CacheCommonUniforms()
{
    if (_programId == 0)
        return;

    _commonUniforms.modelLoc = glGetUniformLocation(_programId, "uModel");
    _commonUniforms.viewLoc = glGetUniformLocation(_programId, "uView");
    _commonUniforms.projectionLoc = glGetUniformLocation(_programId, "uProjection");
    _commonUniforms.textureLoc = glGetUniformLocation(_programId, "uTexture");
    _commonUniforms.colorLoc = glGetUniformLocation(_programId, "uColor");

    // Add to cache to avoid duplicate lookups
    if (_commonUniforms.modelLoc != -1)
        _uniformLocationCache["uModel"] = _commonUniforms.modelLoc;
    if (_commonUniforms.viewLoc != -1)
        _uniformLocationCache["uView"] = _commonUniforms.viewLoc;
    if (_commonUniforms.projectionLoc != -1)
        _uniformLocationCache["uProjection"] = _commonUniforms.projectionLoc;
    if (_commonUniforms.textureLoc != -1)
        _uniformLocationCache["uTexture"] = _commonUniforms.textureLoc;
    if (_commonUniforms.colorLoc != -1)
        _uniformLocationCache["uColor"] = _commonUniforms.colorLoc;
}