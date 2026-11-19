#include "renderer/common/UserShaderGl.hpp"

#include <cstddef>

#include "renderer/common/SceneLighting.hpp"
#include "renderer/common/UserShader.hpp"

namespace Renderer::Common::UserGl
{
    const char* VertexSource()
    {
        // The attribute locations and the transform maths are the lit shader's (OpenGLRenderer.cpp litVert); the normal matrix and the
        // tangent are the Vulkan user.vert's. Matrices arrive row-major (glUniformMatrix4fv with transpose), so the products read as
        // in the built-in shaders
        return R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec4 aColor;
layout (location = 4) in vec4 aTangent;

uniform mat4 n2_Model;
uniform mat4 n2_View;
uniform mat4 n2_Projection;

out vec2 n2_UV;
out vec4 n2_Color;
out vec3 n2_WorldPos;
out vec3 n2_Normal;
out vec4 n2_Tangent;

void main() {
    vec4 worldPos = n2_Model * vec4(aPos, 1.0);
    n2_WorldPos = worldPos.xyz;
    mat3 normalMatrix = transpose(inverse(mat3(n2_Model)));
    n2_Normal = normalize(normalMatrix * aNormal);
    vec3 worldTangent = mat3(n2_Model) * aTangent.xyz;
    float worldTangentLength = length(worldTangent);
    worldTangent = worldTangentLength > 0.0 ? worldTangent / worldTangentLength : vec3(0.0);
    n2_Tangent = vec4(worldTangent, aTangent.w * (determinant(mat3(n2_Model)) < 0.0 ? -1.0 : 1.0));
    n2_UV = aTexCoord;
    n2_Color = aColor;
    gl_Position = n2_Projection * n2_View * worldPos;
}
)";
    }

    std::vector<std::string> PerDrawUniformNames()
    {
        return {kModel, kView, kProjection, kStageDim, kTintSaturation, kTintInside, kTintCircle, kTargetHeight};
    }

    std::string TextureSamplerName(const unsigned slot)
    {
        return "n2_tex" + std::to_string(slot);
    }

    std::string LightMember(const char* array, const int index, const char* member)
    {
        return std::string(array) + "[" + std::to_string(index) + "]." + member;
    }

    std::vector<std::string> LightingUniformNames()
    {
        const LightingNames& names = kUserLightingNames;
        std::vector<std::string> out{names.encodeOutput,   names.ambient,  names.cameraPos,
                                     names.numDirectional, names.numPoint, names.numSpot};
        for (int i = 0; i < SceneLightingData::MAX_DIRECTIONAL_LIGHTS; ++i)
        {
            for (const char* member : {"direction", "color", "intensity"})
            {
                out.push_back(LightMember(names.directionalArray, i, member));
            }
        }
        for (int i = 0; i < SceneLightingData::MAX_POINT_LIGHTS; ++i)
        {
            for (const char* member : {"position", "color", "intensity", "range", "attenuation"})
            {
                out.push_back(LightMember(names.pointArray, i, member));
            }
        }
        for (int i = 0; i < SceneLightingData::MAX_SPOT_LIGHTS; ++i)
        {
            for (const char* member : {"position", "direction", "color", "intensity", "range", "cosInner", "cosOuter"})
            {
                out.push_back(LightMember(names.spotArray, i, member));
            }
        }
        return out;
    }

    std::string ValidateFragmentSource(const std::string& text)
    {
        constexpr const char* version = "#version 330 core";
        if (text.empty())
        {
            return "the OpenGL fragment payload is empty";
        }
        if (text.size() > kUserShaderMaxGlslBytes)
        {
            return "the OpenGL fragment payload is " + std::to_string(text.size()) + " bytes; the limit is " +
                std::to_string(kUserShaderMaxGlslBytes);
        }
        if (text.find('\0') != std::string::npos)
        {
            return "the OpenGL fragment payload is not text (it holds a NUL byte)";
        }
        if (text.rfind(version, 0) != 0)
        {
            return "the OpenGL fragment payload does not start with '#version 330 core'";
        }
        // Every directive line after the first: #extension is never generated (nothing the contract needs), and a second #version
        // would be a way to change the language the driver compiles
        std::size_t at = text.find('\n');
        while (at != std::string::npos)
        {
            std::size_t begin = at + 1;
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
            {
                ++begin;
            }
            if (text.compare(begin, 10, "#extension") == 0)
            {
                return "the OpenGL fragment payload has an #extension line, which the contract does not allow";
            }
            if (text.compare(begin, 8, "#version") == 0)
            {
                return "the OpenGL fragment payload has a second #version line";
            }
            at = text.find('\n', begin);
        }
        if (text.find("void main") == std::string::npos)
        {
            return "the OpenGL fragment payload has no main function";
        }
        return {};
    }
}
