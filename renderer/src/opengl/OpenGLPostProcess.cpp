#include <algorithm>
#include <iostream>
#include <vector>

#include "renderer/opengl/OpenGLRenderer.hpp"

// Post-processing and render textures on OpenGL (issue #445). The passes follow Common::ApplyPostProcessCpu: the frame's target
// is blitted to a texture (m_postScene), the bright pass and the blur passes run on two half-size textures, and the composite
// draws over the frame's target. One GLSL 330 program does every pass (uA.x is the mode) over a full-screen triangle made from
// gl_VertexID (an empty vertex array, as the core profile wants one bound). GL's framebuffer and its textures both have their
// first row at the bottom, so uv is simply the position's.

using namespace Renderer::OpenGL;
using Renderer::Common::PostPassMode;
using Renderer::Common::PostProcessParams;
using Renderer::Common::PostProcessSettings;

namespace
{
    const char *PostVertexSource = R"(
        #version 330 core
        out vec2 vUv;
        void main() {
            vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
            gl_Position = vec4(pos, 0.0, 1.0);
            vUv = pos * 0.5 + 0.5;
        }
    )";

    // The same maths as renderer/shaders/vulkan/post.frag and renderer/shaders/webgpu/post.wgsl
    const char *PostFragmentSource = R"(
        #version 330 core
        in vec2 vUv;
        out vec4 FragColor;
        uniform sampler2D uSource;
        uniform sampler2D uBloom;
        uniform vec4 uA; // mode, threshold, intensity, invert
        uniform vec4 uB; // stepX, stepY, chromatic aberration, vignette
        uniform vec4 uC; // tint rgb, flipY (unused: GL needs no flip)
        void main() {
            int mode = int(uA.x + 0.5);
            vec2 uv = vUv;
            if (mode == 0) {
                FragColor = vec4(texture(uSource, uv).rgb, 1.0);
            } else if (mode == 1) {
                vec3 c = texture(uSource, uv).rgb;
                float brightest = max(c.r, max(c.g, c.b));
                float weight = max(brightest - uA.y, 0.0) / max(brightest, 1e-4);
                FragColor = vec4(c * weight, 1.0);
            } else if (mode == 2) {
                vec2 o1 = uB.xy;
                vec2 o2 = uB.xy * 2.0;
                vec2 o3 = uB.xy * 3.0;
                vec2 o4 = uB.xy * 4.0;
                vec3 sum = texture(uSource, uv).rgb * 0.2270270270;
                sum += (texture(uSource, uv + o1).rgb + texture(uSource, uv - o1).rgb) * 0.1945945946;
                sum += (texture(uSource, uv + o2).rgb + texture(uSource, uv - o2).rgb) * 0.1216216216;
                sum += (texture(uSource, uv + o3).rgb + texture(uSource, uv - o3).rgb) * 0.0540540541;
                sum += (texture(uSource, uv + o4).rgb + texture(uSource, uv - o4).rgb) * 0.0162162162;
                FragColor = vec4(sum, 1.0);
            } else {
                vec2 d = (uv - 0.5) * uB.z;
                vec4 base = texture(uSource, uv);
                vec3 rgb = vec3(texture(uSource, uv + d).r, base.g, texture(uSource, uv - d).b);
                rgb += texture(uBloom, uv).rgb * uA.z * uC.rgb;
                rgb = clamp(rgb, 0.0, 1.0);
                rgb = mix(rgb, 1.0 - rgb, uA.w);
                float distance = length(uv - 0.5) * 1.41421356;
                rgb = clamp(rgb * (1.0 - uB.w * smoothstep(0.3, 1.0, distance)), 0.0, 1.0);
                FragColor = vec4(rgb, base.a);
            }
        }
    )";

    bool MakeTarget(GLuint &texture, GLuint &framebuffer, const uint32_t width, const uint32_t height)
    {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    void SetParams(const GLuint program, const PostProcessParams &params)
    {
        glUniform4fv(glGetUniformLocation(program, "uA"), 1, params.a);
        glUniform4fv(glGetUniformLocation(program, "uB"), 1, params.b);
        glUniform4fv(glGetUniformLocation(program, "uC"), 1, params.c);
    }

    void BindSources(const GLuint source, const GLuint bloom)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloom);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
    }
}

GLuint OpenGLRenderer::FrameTarget(uint32_t &width, uint32_t &height) const
{
    if (m_offscreenFramebuffer != 0)
    {
        width = m_offscreenWidth;
        height = m_offscreenHeight;
        return m_offscreenFramebuffer;
    }
    width = m_width;
    height = m_height;
    return 0;
}

void OpenGLRenderer::DestroyPostTargets()
{
    for (PostTarget *target : {&m_postScene, &m_postBloomA, &m_postBloomB})
    {
        if (target->framebuffer != 0)
        {
            glDeleteFramebuffers(1, &target->framebuffer);
        }
        if (target->texture != 0)
        {
            glDeleteTextures(1, &target->texture);
        }
        *target = PostTarget{};
    }
    m_postWidth = 0;
    m_postHeight = 0;
}

void OpenGLRenderer::DestroyPostResources()
{
    // No GL calls when nothing was made: Shutdown runs this, maybe without GL ever loaded
    if (m_postProgram == 0 && m_postVertexArray == 0 && m_postScene.texture == 0)
    {
        m_renderTextures.clear();
        return;
    }
    DestroyPostTargets();
    if (m_postProgram != 0)
    {
        glDeleteProgram(m_postProgram);
        m_postProgram = 0;
    }
    if (m_postVertexArray != 0)
    {
        glDeleteVertexArrays(1, &m_postVertexArray);
        m_postVertexArray = 0;
    }
    m_postProgramFailed = false;
    m_renderTextures.clear();
}

bool OpenGLRenderer::EnsurePostResources(const uint32_t width, const uint32_t height)
{
    if (m_postProgram == 0)
    {
        if (m_postProgramFailed)
        {
            return false;
        }
        const GLuint vertex = CompileShader(PostVertexSource, GL_VERTEX_SHADER);
        const GLuint fragment = CompileShader(PostFragmentSource, GL_FRAGMENT_SHADER);
        if (vertex != 0 && fragment != 0)
        {
            m_postProgram = LinkProgram(vertex, fragment);
        }
        if (vertex != 0)
        {
            glDeleteShader(vertex);
        }
        if (fragment != 0)
        {
            glDeleteShader(fragment);
        }
        if (m_postProgram == 0)
        {
            m_postProgramFailed = true;
            std::cerr << "OpenGL: the post-processing shader did not build; post-processing is off" << std::endl;
            return false;
        }
        glUseProgram(m_postProgram);
        glUniform1i(glGetUniformLocation(m_postProgram, "uSource"), 0);
        glUniform1i(glGetUniformLocation(m_postProgram, "uBloom"), 1);
        glUseProgram(0);
        glGenVertexArrays(1, &m_postVertexArray);
    }
    if (m_postScene.framebuffer != 0 && m_postWidth == width && m_postHeight == height)
    {
        return true;
    }
    DestroyPostTargets();
    const uint32_t bloomWidth = Common::BloomExtent(width);
    const uint32_t bloomHeight = Common::BloomExtent(height);
    const bool ok = MakeTarget(m_postScene.texture, m_postScene.framebuffer, width, height) &&
        MakeTarget(m_postBloomA.texture, m_postBloomA.framebuffer, bloomWidth, bloomHeight) &&
        MakeTarget(m_postBloomB.texture, m_postBloomB.framebuffer, bloomWidth, bloomHeight);
    if (!ok)
    {
        std::cerr << "OpenGL: could not make the post-processing targets; post-processing is skipped" << std::endl;
        DestroyPostTargets();
        return false;
    }
    m_postWidth = width;
    m_postHeight = height;
    return true;
}

void OpenGLRenderer::RestoreFrameState(const GLuint target, const uint32_t width, const uint32_t height)
{
    glBindVertexArray(0);
    glUseProgram(0);
    m_currentShader = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glPolygonMode(GL_FRONT_AND_BACK, m_wireframeEnabled ? GL_LINE : GL_FILL);
    m_appliedStateValid = false; // the next draw sets its whole state again
}

void OpenGLRenderer::ApplyPostProcess(const PostProcessSettings &input)
{
    const PostProcessSettings settings = PostProcessSettings::Sanitized(input);
    uint32_t width = 0;
    uint32_t height = 0;
    const GLuint target = FrameTarget(width, height);
    if (!settings.IsActive() || width == 0 || height == 0 || !EnsurePostResources(width, height))
    {
        RestoreFrameState(target, width, height);
        return;
    }

    // The passes write colours as they are
    if (m_framebufferSrgbOn)
    {
        glDisable(GL_FRAMEBUFFER_SRGB);
        m_framebufferSrgbOn = false;
    }
    const auto w = static_cast<GLint>(width);
    const auto h = static_cast<GLint>(height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_postScene.framebuffer);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glUseProgram(m_postProgram);
    glBindVertexArray(m_postVertexArray);

    if (settings.BloomActive())
    {
        const uint32_t bloomWidth = Common::BloomExtent(width);
        const uint32_t bloomHeight = Common::BloomExtent(height);
        glViewport(0, 0, static_cast<GLsizei>(bloomWidth), static_cast<GLsizei>(bloomHeight));
        glBindFramebuffer(GL_FRAMEBUFFER, m_postBloomA.framebuffer);
        BindSources(m_postScene.texture, m_postScene.texture);
        SetParams(m_postProgram, PostProcessParams::For(PostPassMode::Bright, settings));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int pass = 0; pass < settings.bloomIterations; ++pass)
        {
            const float spread = static_cast<float>(pass + 1);
            PostProcessParams params = PostProcessParams::For(PostPassMode::Blur, settings);
            params.b[0] = spread / static_cast<float>(bloomWidth);
            params.b[1] = 0.0f;
            glBindFramebuffer(GL_FRAMEBUFFER, m_postBloomB.framebuffer);
            BindSources(m_postBloomA.texture, m_postBloomA.texture);
            SetParams(m_postProgram, params);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            params.b[0] = 0.0f;
            params.b[1] = spread / static_cast<float>(bloomHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, m_postBloomA.framebuffer);
            BindSources(m_postBloomB.texture, m_postBloomB.texture);
            SetParams(m_postProgram, params);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, w, h);
    BindSources(m_postScene.texture, settings.BloomActive() ? m_postBloomA.texture : m_postScene.texture);
    SetParams(m_postProgram, PostProcessParams::For(PostPassMode::Composite, settings));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    BindSources(0, 0);

    RestoreFrameState(target, width, height);
}

Renderer::Common::ITexture *OpenGLRenderer::CreateRenderTexture(const uint32_t width, const uint32_t height)
{
    Common::ITexture *texture = Common::IRenderer::CreateRenderTexture(width, height);
    if (texture == nullptr)
    {
        return nullptr;
    }
    // A capture is opaque: the frame's alpha (under 1 where alpha-blended draws left it) reads as 1
    if (const auto it = m_textures.find(texture); it != m_textures.end())
    {
        glBindTexture(GL_TEXTURE_2D, it->second->GetHandle());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_renderTextures.push_back(texture);
    return texture;
}

bool OpenGLRenderer::CaptureFrame(Common::ITexture *target)
{
    if (target == nullptr || std::find(m_renderTextures.begin(), m_renderTextures.end(), target) == m_renderTextures.end())
    {
        return false;
    }
    const auto it = m_textures.find(target);
    if (it == m_textures.end())
    {
        return false;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    const GLuint frame = FrameTarget(width, height);
    if (width == 0 || height == 0)
    {
        return false;
    }
    if (m_framebufferSrgbOn)
    {
        glDisable(GL_FRAMEBUFFER_SRGB);
        m_framebufferSrgbOn = false;
    }
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second->GetHandle(), 0);
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete)
    {
        // Both have their first row at the bottom, so the texture comes out upright on the engine's v-up UVs
        glBindFramebuffer(GL_READ_FRAMEBUFFER, frame);
        glBlitFramebuffer(0, 0, static_cast<GLint>(width), static_cast<GLint>(height), 0, 0,
                          static_cast<GLint>(target->GetWidth()), static_cast<GLint>(target->GetHeight()),
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glDeleteFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, frame);
    return complete;
}
