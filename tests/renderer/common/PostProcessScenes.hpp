#pragma once

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/PostProcess.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>

// Post-processing and render textures (issue #445) checked the same way on every backend that CI runs (the software renderer,
// Vulkan on lavapipe, WebGPU on Dawn over lavapipe): each check draws a small scene on an initialized renderer whose frames
// are Size x Size, reads it back, and compares it with Common::ApplyPostProcessCpu (the reference the GPU shaders implement)
// or with what a capture must hold. The backend's own test file calls them from its fixture.
namespace PostProcessScenes
{
    using namespace Renderer::Common;

    inline constexpr int Size = 64;
    inline constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    using Pixel = std::array<int, 4>;

    struct Image
    {
        std::vector<std::uint8_t> rgba; // row 0 at the bottom (ReadFramebuffer's order)
        [[nodiscard]] Pixel At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + x) * 4;
            return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
        }
    };

    /// A quad from (x0, y0) to (x1, y1) in clip space, uv 0..1 across it (v up)
    inline MeshData Quad(const float x0, const float y0, const float x1, const float y1)
    {
        MeshData data;
        data.vertices = {
            Vertex{{x0, y0, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{x1, y0, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{x1, y1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
            Vertex{{x0, y1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    /// Drawn in submission order on every backend: no depth, no culling, no blending
    inline RenderState Flat()
    {
        RenderState state;
        state.depthTest = false;
        state.depthWrite = false;
        state.cull = CullMode::None;
        state.blend = false;
        return state;
    }

    /// The largest channel difference between two images (alpha too when `alpha`)
    inline int MaxDifference(const Image &a, const Image &b, const bool alpha = false)
    {
        int worst = 0;
        for (std::size_t i = 0; i < a.rgba.size(); ++i)
        {
            if (!alpha && i % 4 == 3)
            {
                continue;
            }
            worst = std::max(worst, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
        }
        return worst;
    }

    /// What ApplyPostProcessCpu makes of `frame`
    inline Image Reference(const Image &frame, const PostProcessSettings &settings)
    {
        std::vector<std::uint32_t> packed(static_cast<std::size_t>(Size) * Size);
        for (std::size_t i = 0; i < packed.size(); ++i)
        {
            packed[i] = std::uint32_t(frame.rgba[i * 4]) | (std::uint32_t(frame.rgba[i * 4 + 1]) << 8) |
                (std::uint32_t(frame.rgba[i * 4 + 2]) << 16) | (std::uint32_t(frame.rgba[i * 4 + 3]) << 24);
        }
        ApplyPostProcessCpu(packed.data(), Size, Size, settings);
        Image out{std::vector<std::uint8_t>(frame.rgba.size())};
        for (std::size_t i = 0; i < packed.size(); ++i)
        {
            for (int c = 0; c < 4; ++c)
            {
                out.rgba[i * 4 + c] = static_cast<std::uint8_t>((packed[i] >> (8 * c)) & 0xFFu);
            }
        }
        return out;
    }

    class Scenes
    {
    public:
        explicit Scenes(IRenderer &renderer) : _r(renderer)
        {
            _unlit = _r.GetStandardUnlitShader();
            _white = Colour(1, 1, 1);
            _red = Colour(1, 0, 0);
            _blue = Colour(0, 0, 1);
            _green = Colour(0, 1, 0);
            _grey = Colour(0.5f, 0.25f, 0.75f);
            _full = _r.CreateMesh(Quad(-1, -1, 1, 1));
            _centre = _r.CreateMesh(Quad(-0.125f, -0.125f, 0.125f, 0.125f));
            _bottomLeft = _r.CreateMesh(Quad(-1, -1, 0, 0));
            _topRight = _r.CreateMesh(Quad(0, 0, 1, 1));
            _corner = _r.CreateMesh(Quad(-1, -1, -0.75f, -0.75f));
        }

        [[nodiscard]] bool Ready() const
        {
            return _unlit && _white && _red && _blue && _green && _grey && _full && _centre && _bottomLeft && _topRight &&
                _corner;
        }

        IMaterial *Colour(const float r, const float g, const float b)
        {
            IMaterial *material = _unlit ? _r.CreateMaterial(_unlit) : nullptr;
            if (material)
            {
                material->SetColor("uAlbedo", r, g, b, 1.0f);
            }
            return material;
        }

        template <class Draws> Image Frame(const Draws &draws, const float clearAlpha = 1.0f)
        {
            _r.Clear(0, 0, 0, clearAlpha);
            _r.BeginFrame();
            _r.SetViewProjection(Identity, Identity);
            draws();
            _r.EndFrame();
            Image image{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
            _r.ReadFramebuffer(image.rgba.data(), Size, Size);
            return image;
        }

        /// A white 8x8 square in the middle, bloomed: the GPU frame matches the CPU reference, glows past the square and
        /// leaves the far corner dark
        void CheckBloom(const int tolerance)
        {
            PostProcessSettings settings;
            settings.bloom = true;
            settings.bloomThreshold = 0.5f;
            settings.bloomIntensity = 4.0f;
            settings.bloomIterations = 3;
            const Image plain = Frame([&] { _r.DrawMesh(_centre, Identity, _white, Flat()); });
            const Image bloomed = Frame([&] {
                _r.DrawMesh(_centre, Identity, _white, Flat());
                _r.ApplyPostProcess(settings);
            });
            ASSERT_EQ(plain.At(32, 32)[0], 255);
            EXPECT_EQ(plain.At(40, 32)[0], 0);
            EXPECT_GT(bloomed.At(40, 32)[0], 20) << "no glow 4 pixels past the square";
            EXPECT_LE(bloomed.At(0, 0)[0], 2) << "the glow reached the far corner";
            EXPECT_EQ(bloomed.At(32, 32)[0], 255);
            EXPECT_LE(MaxDifference(bloomed, Reference(plain, settings)), tolerance);

            // A tint colours the glow only
            settings.bloomTint[0] = 0.0f;
            settings.bloomTint[1] = 0.0f;
            const Image tinted = Frame([&] {
                _r.DrawMesh(_centre, Identity, _white, Flat());
                _r.ApplyPostProcess(settings);
            });
            EXPECT_LE(tinted.At(40, 32)[0], 2);
            EXPECT_GT(tinted.At(40, 32)[2], 20);
            EXPECT_LE(MaxDifference(tinted, Reference(plain, settings)), tolerance);
        }

        /// Invert, vignette and chromatic aberration over a coloured frame match the reference
        void CheckComposite(const int tolerance)
        {
            PostProcessSettings settings;
            settings.invert = 1.0f;
            settings.vignette = 0.8f;
            settings.chromaticAberration = 0.05f;
            const auto draw = [&] {
                _r.DrawMesh(_full, Identity, _grey, Flat());
                _r.DrawMesh(_centre, Identity, _white, Flat());
            };
            const Image plain = Frame(draw);
            const Image processed = Frame([&] {
                draw();
                _r.ApplyPostProcess(settings);
            });
            EXPECT_LE(MaxDifference(processed, Reference(plain, settings)), tolerance);
            // Inverted grey (0.5, 0.25, 0.75) near the centre, darker at the corners
            EXPECT_NEAR(processed.At(20, 32)[1], 191, tolerance + 1);
            EXPECT_LT(processed.At(0, 0)[1], processed.At(20, 32)[1] - 60);
            EXPECT_EQ(processed.At(10, 10)[3], plain.At(10, 10)[3]) << "alpha is the frame's";

            // An inactive setting changes nothing
            const Image untouched = Frame([&] {
                draw();
                _r.ApplyPostProcess(PostProcessSettings{});
            });
            EXPECT_EQ(MaxDifference(untouched, plain, true), 0);
        }

        /// The blur (issue #445): a white square spreads past its edges and fades in the middle, the frame matches the CPU
        /// reference at full and part strength, alone and before the composite's effects
        void CheckBlur(const int tolerance)
        {
            const auto draw = [&] { _r.DrawMesh(_centre, Identity, _white, Flat()); };
            const Image plain = Frame(draw);
            const auto processed = [&](const PostProcessSettings &settings) {
                return Frame([&] {
                    draw();
                    _r.ApplyPostProcess(settings);
                });
            };

            PostProcessSettings settings;
            settings.blur = true;
            settings.blurIterations = 2;
            const Image blurred = processed(settings);
            EXPECT_GT(blurred.At(38, 32)[0], 10) << "nothing spread past the square";
            EXPECT_LT(blurred.At(35, 32)[0], 240) << "the edge stayed sharp";
            EXPECT_LE(blurred.At(0, 0)[0], 2);
            EXPECT_LE(MaxDifference(blurred, Reference(plain, settings)), tolerance);

            settings.blurStrength = 0.5f;
            const Image half = processed(settings);
            EXPECT_NEAR(half.At(38, 32)[0], blurred.At(38, 32)[0] / 2, tolerance + 1);
            EXPECT_LE(MaxDifference(half, Reference(plain, settings)), tolerance);

            // The other effects see the blurred frame
            settings.blurStrength = 1.0f;
            settings.invert = 1.0f;
            const Image inverted = processed(settings);
            EXPECT_NEAR(inverted.At(38, 32)[0], 255 - blurred.At(38, 32)[0], tolerance + 1);
            EXPECT_LE(MaxDifference(inverted, Reference(plain, settings)), tolerance);

            settings.invert = 0.0f;
            settings.bloom = true;
            settings.bloomThreshold = 0.2f;
            settings.bloomIntensity = 2.0f;
            EXPECT_LE(MaxDifference(processed(settings), Reference(plain, settings)), tolerance);

            // No strength, no change
            PostProcessSettings off;
            off.blur = true;
            off.blurStrength = 0.0f;
            EXPECT_EQ(MaxDifference(processed(off), plain, true), 0);
        }

        /// The warp (issue #445): the frame matches the CPU reference for a barrel, a pinch and a wave, which moves a
        /// picture that is not symmetric, so the backends' row orders are checked; the corners stay put
        void CheckWarp(const int tolerance)
        {
            const auto draw = [&] {
                _r.DrawMesh(_bottomLeft, Identity, _blue, Flat());
                _r.DrawMesh(_centre, Identity, _white, Flat());
                _r.DrawMesh(_corner, Identity, _red, Flat());
            };
            const Image plain = Frame(draw);
            const auto processed = [&](const PostProcessSettings &settings) {
                return Frame([&] {
                    draw();
                    _r.ApplyPostProcess(settings);
                });
            };

            PostProcessSettings barrel;
            barrel.distortion = 1.0f;
            const Image bulged = processed(barrel);
            EXPECT_LE(MaxDifference(bulged, Reference(plain, barrel)), tolerance);
            ASSERT_EQ(plain.At(38, 32)[0], 0);
            EXPECT_GT(bulged.At(38, 32)[0], 200) << "the middle square did not grow";
            EXPECT_EQ(bulged.At(1, 1)[0], plain.At(1, 1)[0]) << "the corner stays";

            PostProcessSettings pinch;
            pinch.distortion = -0.8f;
            EXPECT_LE(MaxDifference(processed(pinch), Reference(plain, pinch)), tolerance);

            PostProcessSettings wave;
            wave.waveAmplitude = 0.05f;
            wave.waveFrequency = 1.5f;
            wave.wavePhase = 0.2f;
            const Image waved = processed(wave);
            EXPECT_GT(MaxDifference(waved, plain), 100) << "nothing moved";
            EXPECT_LE(MaxDifference(waved, Reference(plain, wave)), tolerance);

            // With the blur before it and the composite after
            wave.blur = true;
            wave.vignette = 0.5f;
            wave.distortion = 0.3f;
            EXPECT_LE(MaxDifference(processed(wave), Reference(plain, wave)), tolerance);
        }

        /// What is drawn after the post-process (the UI) is not processed, and the draws before it are
        void CheckLaterDrawsAreUntouched()
        {
            PostProcessSettings settings;
            settings.invert = 1.0f;
            const Image frame = Frame([&] {
                _r.DrawMesh(_centre, Identity, _white, Flat());
                _r.ApplyPostProcess(settings);
                _r.DrawMesh(_corner, Identity, _red, Flat());
            });
            EXPECT_EQ(frame.At(2, 2), (Pixel{255, 0, 0, 255})) << "the draw after the post-process was processed";
            EXPECT_EQ(frame.At(32, 32)[0], 0) << "the square was not inverted";
            EXPECT_EQ(frame.At(50, 50)[0], 255) << "the black background was not inverted";
        }

        /// A capture is upright (its first row the bottom of the frame), opaque, scaled, and visible to the frame's later draws
        void CheckCapture(const int tolerance)
        {
            ITexture *same = _r.CreateRenderTexture(Size, Size);
            ITexture *small = _r.CreateRenderTexture(16, 16);
            ASSERT_NE(same, nullptr);
            ASSERT_NE(small, nullptr);
            EXPECT_EQ(same->GetWidth(), static_cast<std::uint32_t>(Size));
            IMaterial *showSame = _r.CreateMaterial(_unlit, same);
            IMaterial *showSmall = _r.CreateMaterial(_unlit, small);
            ASSERT_NE(showSame, nullptr);
            ASSERT_NE(showSmall, nullptr);
            for (IMaterial *material : {showSame, showSmall})
            {
                material->SetColor("uAlbedo", 1, 1, 1, 1);
                material->SetInt("uHasTexture", 1);
            }

            // Frame 1: blue bottom left, green top right, a transparent clear elsewhere; captured into both textures
            bool capturedSame = false, capturedSmall = false;
            const Image original = Frame(
                [&] {
                    _r.DrawMesh(_bottomLeft, Identity, _blue, Flat());
                    _r.DrawMesh(_topRight, Identity, _green, Flat());
                    capturedSame = _r.CaptureFrame(same);
                    capturedSmall = _r.CaptureFrame(small);
                },
                0.0f);
            EXPECT_TRUE(capturedSame);
            EXPECT_TRUE(capturedSmall);

            // Frame 2: each texture over the whole frame shows frame 1 upright
            const Image shownSame = Frame([&] { _r.DrawMesh(_full, Identity, showSame, Flat()); });
            EXPECT_LE(MaxDifference(shownSame, original), tolerance);
            EXPECT_EQ(shownSame.At(8, 8)[2], 255) << "blue is not at the bottom left: the capture is upside down";
            EXPECT_EQ(shownSame.At(56, 56)[1], 255);
            EXPECT_EQ(shownSame.At(8, 56)[3], 255) << "a capture is opaque";
            const Image shownSmall = Frame([&] { _r.DrawMesh(_full, Identity, showSmall, Flat()); });
            EXPECT_GE(shownSmall.At(8, 8)[2], 250);
            EXPECT_GE(shownSmall.At(56, 56)[1], 250);
            EXPECT_LE(shownSmall.At(8, 56)[1] + shownSmall.At(8, 56)[2], 4);

            // Within one frame: a capture, then the texture drawn into the top right quarter, shows what was drawn before it
            const Image same_frame = Frame([&] {
                _r.DrawMesh(_bottomLeft, Identity, _red, Flat());
                _r.CaptureFrame(small);
                _r.DrawMesh(_topRight, Identity, showSmall, Flat());
            });
            EXPECT_GE(same_frame.At(40, 40)[0], 250) << "the later draw did not see the capture";
            EXPECT_LE(same_frame.At(56, 56)[0], 4);

            // Only render textures can be captured into
            const std::uint8_t pixel[4] = {0, 0, 0, 255};
            ITexture *plain = _r.CreateTexture(pixel, 1, 1, 4);
            ASSERT_NE(plain, nullptr);
            bool capturedPlain = true;
            Frame([&] { capturedPlain = _r.CaptureFrame(plain); });
            EXPECT_FALSE(capturedPlain);
            EXPECT_EQ(_r.CreateRenderTexture(0, 4), nullptr);
            EXPECT_EQ(_r.CreateRenderTexture(MaxRenderTextureSize + 1, 4), nullptr);
            _r.DestroyTexture(plain);
            _r.DestroyMaterial(showSame);
            _r.DestroyMaterial(showSmall);
            _r.DestroyTexture(same);
            _r.DestroyTexture(small);
        }

    private:
        IRenderer &_r;
        IShader *_unlit = nullptr;
        IMaterial *_white = nullptr, *_red = nullptr, *_blue = nullptr, *_green = nullptr, *_grey = nullptr;
        IMesh *_full = nullptr, *_centre = nullptr, *_bottomLeft = nullptr, *_topRight = nullptr, *_corner = nullptr;
    };
}
