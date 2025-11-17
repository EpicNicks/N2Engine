#include <iostream>

#include "renderer/opengl/OpenGLTexture.hpp"

using namespace Renderer::OpenGL;

OpenGLTexture::~OpenGLTexture()
{
    Destroy();
}

bool OpenGLTexture::Initialize(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels)
{
    if (!data || width == 0 || height == 0 || channels == 0)
        return false;

    m_width = width;
    m_height = height;
    m_channels = channels;

    glGenTextures(1, &m_handle);
    glBindTexture(GL_TEXTURE_2D, m_handle);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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
        internalFormat = GL_RGB8;
        break;
    case 4:
        format = GL_RGBA;
        internalFormat = GL_RGBA8;
        break;
    default:
        format = GL_RGB;
        internalFormat = GL_RGB8;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

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