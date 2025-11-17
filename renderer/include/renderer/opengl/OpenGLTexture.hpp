#pragma once

#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "renderer/common/ITexture.hpp"

namespace Renderer::OpenGL
{
    class OpenGLTexture : public Common::ITexture
    {
    public:
        OpenGLTexture() = default;
        ~OpenGLTexture() override;

        bool Initialize(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels);
        void Destroy();

        bool IsValid() const override { return m_isValid; }
        uint32_t GetWidth() const override { return m_width; }
        uint32_t GetHeight() const override { return m_height; }
        uint32_t GetChannels() const override { return m_channels; }

        // OpenGL-specific accessor
        GLuint GetHandle() const { return m_handle; }

    private:
        GLuint m_handle = 0;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_channels = 0;
        bool m_isValid = false;
    };
}