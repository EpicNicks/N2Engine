#include "SampleConsoleHost.hpp"

#include <cstdio>

#include <renderer/software/SoftwareRenderer.hpp>

namespace SampleConsole
{
    namespace
    {
        /// The sample's own renderer: the engine's software renderer, under the target's name. A real target would
        /// implement Renderer::Common::IRenderer over its GPU API here; the engine takes it through CreateRenderer.
        class SampleRenderer final : public Renderer::Software::SoftwareRenderer
        {
        public:
            SampleRenderer()
            {
                std::printf("sample-console: the platform's own renderer\n");
                std::fflush(stdout);
            }
        };

        /// A handle no other kind uses: the backend and its renderer agree on what it is (here, the host itself)
        void *HandleOf(const SampleConsoleHost *host)
        {
            return const_cast<SampleConsoleHost *>(host);
        }
    }

    N2Engine::Platform::NativeSurface SampleConsoleHost::Surface() const
    {
        return {N2Engine::Platform::NativeSurface::Kind::Custom, HandleOf(this), nullptr, Width, Height};
    }

    Renderer::Common::Surface SampleConsoleHost::RenderSurface() const
    {
        Renderer::Common::Surface surface;
        surface.kind = Renderer::Common::Surface::Kind::Custom;
        surface.handle = HandleOf(this);
        surface.presentPixels = [this](const std::uint8_t *rgba, const int width, const int height) {
            const_cast<SampleConsoleHost *>(this)->Show(rgba, width, height);
        };
        surface.framebufferSize = [] { return std::pair<int, int>{Width, Height}; };
        return surface;
    }

    std::unique_ptr<Renderer::Common::IRenderer> SampleConsoleHost::CreateRenderer()
    {
        return std::make_unique<SampleRenderer>();
    }

    void SampleConsoleHost::Show(const std::uint8_t *rgba, const int width, const int height)
    {
        ++_frames;
        if (_frames % 30 != 0 || rgba == nullptr)
        {
            return;
        }
        std::uint64_t hash = 1469598103934665603ull;
        const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        for (std::size_t i = 0; i < size; ++i)
        {
            hash = (hash ^ rgba[i]) * 1099511628211ull;
        }
        std::printf("sample-console: frame %d, %dx%d, pixels %016llx\n", _frames, width, height, static_cast<unsigned long long>(hash));
        std::fflush(stdout);
    }
}
