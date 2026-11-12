#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace Renderer::Common
{
    /**
     * What a renderer draws on (build targets stage 7; docs/platform.html#surfaces): the platform's native surface and the
     * few operations a renderer needs from the window system, supplied by the platform backend so no renderer calls one
     * itself. The desktop backend fills them from GLFW, Android's from EGL and ANativeWindow, Apple's from a CAMetalLayer.
     *
     * A default Surface (or nullptr) is no surface: the renderer draws offscreen (headless, the web) and frames are read back.
     */
    struct Surface
    {
        enum class Kind
        {
            /// Offscreen: nothing to present to
            None,
            /// A desktop window (GLFW's today): handle is the platform's window
            DesktopWindow,
            /// An Android ANativeWindow*
            AndroidWindow,
            /// A CAMetalLayer* (iOS, macOS)
            MetalLayer,
            /// A private target's own kind (stage 11)
            Custom,
        };

        Kind kind = Kind::None;
        void *handle = nullptr;
        /// A second handle when the kind needs one (an X11 Display*); null otherwise
        void *display = nullptr;

        // ---- OpenGL (and the software renderer's present, which draws its frame with OpenGL): the window's context
        /// Makes the surface's OpenGL context current on this thread
        std::function<void()> makeContextCurrent;
        /// An OpenGL function's address (glad loads through it)
        std::function<void *(const char *name)> getProcAddress;
        /// Shows the frame drawn into the context
        std::function<void()> swapBuffers;

        // ---- A CPU frame (the software renderer's present per platform)
        /// Shows a finished frame from the CPU: RGBA8, width x height, rows bottom to top (ReadFramebuffer's layout). Set
        /// where copying pixels into the window is the way to present (Android's ANativeWindow buffer); the software
        /// renderer then uses it instead of an OpenGL context. Empty: present through OpenGL, or not at all.
        std::function<void(const std::uint8_t *rgba, int width, int height)> presentPixels;

        // ---- Vulkan
        /// The instance extensions the window system needs (VK_KHR_surface and the platform's own)
        std::function<std::vector<const char *>()> vulkanInstanceExtensions;
        /// Makes a VkSurfaceKHR for this window on instance (a VkInstance); returns the VkResult, the surface written to
        /// *surfaceOut (a VkSurfaceKHR*)
        std::function<int(void *instance, void *surfaceOut)> createVulkanSurface;

        // ---- both
        /// The surface's size in pixels now (a swapchain asks after a resize; 0 x 0 while minimised)
        std::function<std::pair<int, int>()> framebufferSize;

        Surface() = default;
        /// No surface (Initialize(nullptr, width, height): an offscreen renderer)
        Surface(std::nullptr_t) {}

        [[nodiscard]] bool HasWindow() const { return kind != Kind::None && handle != nullptr; }
    };
}
