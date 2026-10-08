#include <iostream>

#include "renderer/opengl/OpenGLTexture.hpp"

using namespace Renderer::OpenGL;

namespace
{
    GLint MinFilter(const Renderer::Common::TextureOptions &options)
    {
        const bool linear = options.filter == Renderer::Common::TextureFilter::Linear;
        if (options.mipmaps)
        {
            return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        }
        return linear ? GL_LINEAR : GL_NEAREST;
    }

    GLint MagFilter(const Renderer::Common::TextureOptions &options)
    {
        return options.filter == Renderer::Common::TextureFilter::Linear ? GL_LINEAR : GL_NEAREST;
    }

    GLint Wrap(const Renderer::Common::TextureOptions &options)
    {
        return options.wrap == Renderer::Common::TextureWrap::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    }
}

OpenGLTexture::~OpenGLTexture()
{
    Destroy();
}

bool OpenGLTexture::Initialize(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels,
                               const Common::TextureOptions &options)
{
    if (!data || width == 0 || height == 0 || channels == 0)
        return false;

    m_width = width;
    m_height = height;
    m_channels = channels;
    m_options = options;

    glGenTextures(1, &m_handle);
    glBindTexture(GL_TEXTURE_2D, m_handle);

    // Set texture parameters (the default options give REPEAT, LINEAR_MIPMAP_LINEAR and LINEAR, as
    // every texture had before options existed)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, Wrap(options));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, Wrap(options));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, MinFilter(options));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, MagFilter(options));

    // Determine format
    GLenum format, internalFormat;
    switch (channels)
    {
    case 1:
        format = GL_RED;
        internalFormat = GL_R8;
        break;
    case 2:
        format = GL_RG;
        internalFormat = GL_RG8;
        break;
    case 3:
        format = GL_RGB;
        internalFormat = options.srgb ? GL_SRGB8 : GL_RGB8;
        break;
    case 4:
        format = GL_RGBA;
        internalFormat = options.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        break;
    default:
        format = GL_RGB;
        internalFormat = GL_RGB8;
    }

    // Rows are tightly packed. GL assumes 4-byte aligned rows by default, which misreads 1- to
    // 3-channel data whose row size isn't a multiple of 4.
    GLint previousAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, format, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);

    if (options.mipmaps)
    {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    m_isValid = true;
    return true;
}

void OpenGLTexture::Destroy()
{
    if (m_isValid)
    {
        glDeleteTextures(1, &m_handle);
        m_handle = 0;
        m_isValid = false;
    }
}
