// Browser-only integration test: callbacks must complete through the real event
// loop. The native synchronous tests and row-padding unit test cannot prove this.
#include <emscripten.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <renderer/common/Renderer.hpp>
#include <renderer/webgpu/WebGpuFactory.hpp>

EM_JS(void, ReportReadbackResult, (int status, const char* message), {
    var report = Module['n2ReadbackResult'];
    if (report) report(status, UTF8ToString(message));
});

namespace
{
    using namespace Renderer::Common;
    std::unique_ptr<IRenderer> renderer;
    std::vector<std::uint8_t> pixels;
    int phase = 0;
    int frames = 0;
    constexpr float identity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    void Finish(int status, const char* message)
    {
        renderer.reset();
        emscripten_cancel_main_loop();
        std::printf("N2_WEBGPU_READBACK status=%d %s\n", status, message);
        ReportReadbackResult(status, message);
    }

    bool Solid(const std::array<std::uint8_t, 4>& expected)
    {
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            if (pixels[i] != expected[i % 4]) return false;
        }
        return true;
    }

    bool Split(int width, int height)
    {
        // Check rows well away from the rasterisation boundary. Row zero must
        // be the BOTTOM of the image, including when the caller resamples it.
        for (int y = 0; y < height; ++y)
        {
            if (y >= height / 4 && y <= 3 * height / 4) continue;
            for (int x = 0; x < width; ++x)
            {
                if (x >= width / 4 && x <= 3 * width / 4) continue;
                const bool left = x < width / 4;
                const bool bottom = y < height / 4;
                const std::array<std::uint8_t, 4> expected = bottom
                    ? (left ? std::array<std::uint8_t, 4>{0, 0, 255, 255}
                            : std::array<std::uint8_t, 4>{255, 255, 0, 255})
                    : (left ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                            : std::array<std::uint8_t, 4>{0, 255, 0, 255});
                const auto index = static_cast<std::size_t>((y * width + x) * 4);
                if (!std::equal(expected.begin(), expected.end(), pixels.begin() + index)) return false;
            }
        }
        return true;
    }

    void Clear(float r, float g, float b, float a)
    {
        renderer->Clear(r, g, b, a);
        renderer->BeginFrame();
        renderer->EndFrame();
    }

    bool DrawSplit()
    {
        MeshData data;
        constexpr std::array<std::array<float, 4>, 4> colours = {{{1, 0, 0, 1}, {0, 1, 0, 1},
                                                                {0, 0, 1, 1}, {1, 1, 0, 1}}};
        for (int quadrant = 0; quadrant < 4; ++quadrant)
        {
            const float left = quadrant % 2 == 0 ? -1.0f : 0.0f;
            const float low = quadrant < 2 ? 0.0f : -1.0f;
            const float high = low + 1.0f;
            for (const auto& xy : std::array<std::array<float, 2>, 4>{{{left, low}, {left + 1, low},
                                                                     {left + 1, high}, {left, high}}})
            {
                Vertex vertex{};
                vertex.position[0] = xy[0];
                vertex.position[1] = xy[1];
                vertex.normal[2] = 1.0f;
                std::copy(colours[quadrant].begin(), colours[quadrant].end(), vertex.color);
                data.vertices.push_back(vertex);
            }
            const auto start = static_cast<std::uint32_t>(quadrant * 4);
            for (const auto index : {0u, 1u, 2u, 2u, 3u, 0u}) data.indices.push_back(start + index);
        }
        IMesh* mesh = renderer->CreateMesh(data);
        IShader* shader = renderer->GetStandardUnlitShader();
        IMaterial* material = shader ? renderer->CreateMaterial(shader) : nullptr;
        if (!mesh || !material) return false;
        material->SetColor("uAlbedo", 1, 1, 1, 1);
        RenderState state;
        state.cull = CullMode::None;
        renderer->SetViewProjection(identity, identity);
        renderer->BeginFrame();
        renderer->DrawMesh(mesh, identity, material, state);
        renderer->EndFrame();
        // Keep resources until renderer destruction, including while commands
        // and a readback copy are in flight.
        return true;
    }

    void Poll()
    {
        if (++frames > 180)
        {
            Finish(1, "asynchronous readback did not return the expected pixels within 180 frames");
            return;
        }
        if (phase == 0)
        {
            renderer->ReadFramebuffer(pixels.data(), 63, 37);
            if (!Solid({255, 0, 0, 128})) return;
            std::puts("readback: unaligned rows and clear alpha passed");
            if (!DrawSplit()) { Finish(1, "could not create split-colour draw resources"); return; }
            phase = 1;
        }
        else if (phase == 1)
        {
            renderer->ReadFramebuffer(pixels.data(), 63, 37);
            if (!Split(63, 37)) return;
            pixels.assign(31 * 19 * 4, 0xA5);
            renderer->ReadFramebuffer(pixels.data(), 31, 19);
            if (!Split(31, 19)) { Finish(1, "resampled readback has the wrong pixels or row order"); return; }
            std::puts("readback: bottom-first rows and resampling passed");
            if (!renderer->SetRenderTargetSize(17, 65)) { Finish(1, "could not resize target"); return; }
            Clear(0, 1, 0, 1);
            pixels.assign(17 * 65 * 4, 0xA5);
            phase = 2;
        }
        else if (phase == 2)
        {
            renderer->ReadFramebuffer(pixels.data(), 17, 65);
            if (!Solid({0, 255, 0, 255})) return;
            std::puts("readback: resize with an earlier copy in flight passed");
            // ReadFramebuffer just started another asynchronous copy. Destroy
            // its renderer now, then let late map callbacks run before success.
            renderer.reset();
            phase = 3;
        }
        else if (++phase >= 6)
        {
            Finish(0, "clear, padding, orientation, resampling, resize and pending-map destruction passed");
        }
    }
}

int main()
{
    renderer = Renderer::WebGpu::CreateWebGpuRenderer();
    if (!renderer || !renderer->Initialize(nullptr, 63, 37))
    {
        Finish(1, "could not initialise the browser WebGPU renderer");
        return 1;
    }
    Clear(1, 0, 0, 0.5f);
    pixels.assign(63 * 37 * 4, 0xA5);
    renderer->ReadFramebuffer(pixels.data(), 63, 37);
    if (!Solid({0xA5, 0xA5, 0xA5, 0xA5}))
    {
        Finish(1, "first readback must leave the destination untouched until its map completes");
        return 1;
    }
    emscripten_set_main_loop(Poll, 0, EM_TRUE);
}
