#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <text/TextLayout.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/text/TextEffects.hpp"

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTextTestUtil.hpp"

// The engine-level text scenes the Vulkan text golden tests share (#43, stage E): a TextRenderer in a scene of its own, drawn by
// an orthographic camera that fits its layout with a margin for the effects, on ANY renderer (the Vulkan renderer under test
// or a headless software renderer for the cross-backend comparison), and read back.
//
// The software tests (tests/engine/rendering/SoftwareTextEffectsGoldenTests.cpp and SoftwareTextPassesGoldenTests.cpp) own a
// SoftwareRenderer in their scene class; here the renderer is given, so the same scene description runs on both backends, at
// any frame size: the margin and the text scale with the frame, so a larger frame is the same picture with more pixels per
// glyph (the cross-backend comparisons use one, so that glyph interiors exist away from the edges).

namespace VulkanTesting::Ink
{
    /// A frame read back from `renderer`, RGBA8 with row 0 at the bottom
    inline Draw::Frame ReadBack(const Renderer::Common::IRenderer& renderer, const int width, const int height)
    {
        Draw::Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4), width,
                          height};
        renderer.ReadFramebuffer(frame.rgba.data(), width, height);
        return frame;
    }

    /// Text in yellow (by default "Hi", spaced out so no effect reaches the next glyph), drawn by a TextRenderer
    /// (depth-tested, in the Transparent queue, as in a scene) through an orthographic camera that fits the layout with a
    /// margin wide enough for the effects. The frame is `width` x `height`; the margin is 20 pixels at 160 wide and scales
    /// with it.
    class EffectsScene
    {
    public:
        explicit EffectsScene(Renderer::Common::IRenderer& renderer, const int width = 160, const int height = 96,
                              const char* content = "Hi", const float letterSpacing = 0.4f)
            : _renderer(renderer), _width(width), _height(height)
        {
            _renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            _object = N2Engine::GameObject::Create("Effects");
            _text = _object->AddComponent<N2Engine::Rendering::TextRenderer>();
            _text->SetText(content);
            _text->SetColor(N2Engine::Common::Color::Yellow);
            _text->SetLetterSpacing(letterSpacing);

            const N2Engine::Text::Rect bounds = _text->GetLayout().bounds;
            const float margin = 20.0f * static_cast<float>(width) / 160.0f;
            const float w = static_cast<float>(width);
            const float h = static_cast<float>(height);
            _scale = std::min((w - 2.0f * margin) / bounds.Width(), (h - 2.0f * margin) / bounds.Height());
            const float left = (bounds.minX + bounds.maxX) * 0.5f - w * 0.5f / _scale;
            const float bottom = (bounds.minY + bounds.maxY) * 0.5f - h * 0.5f / _scale;
            _camera.SetPosition(N2Engine::Math::Vector3(0.0f, 0.0f, 5.0f));
            _camera.SetOrthographic(left, left + w / _scale, bottom, bottom + h / _scale, 0.1f, 100.0f);
        }

        ~EffectsScene()
        {
            if (_text != nullptr)
            {
                _text->CleanupRenderResources(&_renderer);
            }
        }

        EffectsScene(const EffectsScene&) = delete;
        EffectsScene& operator=(const EffectsScene&) = delete;

        [[nodiscard]] bool Ok() const { return _text != nullptr; }
        /// Pixels per em (the font size is 1 world unit)
        [[nodiscard]] float Scale() const { return _scale; }
        [[nodiscard]] N2Engine::Rendering::TextRenderer* TextComponent() { return _text; }

        Draw::Frame Render(const N2Engine::Text::TextEffects& effects)
        {
            _text->SetEffects(effects);
            _renderer.BeginFrame();
            _renderer.SetViewProjection(_camera.GetViewMatrix().Data(), _camera.GetProjectionMatrix().Data());
            _text->Render(&_renderer);
            _renderer.EndFrame();
            _renderer.Present();
            return ReadBack(_renderer, _width, _height);
        }

    private:
        Renderer::Common::IRenderer& _renderer;
        int _width;
        int _height;
        N2Engine::GameObject::Ptr _object;
        N2Engine::Rendering::TextRenderer* _text = nullptr;
        N2Engine::Camera _camera;
        float _scale = 1.0f;
    };

    // ---- Reading frames of several antialiased layers ------------------------------------------------------------------

    /// A pixel of antialiased layers over a black background, of which the face is yellow and the outline blue: the red
    /// channel equals the green one (the shadow's red would break that), the blue channel is the outline's
    inline bool IsRedFree(const Draw::Rgb& pixel)
    {
        return std::abs(static_cast<int>(pixel.r) - static_cast<int>(pixel.g)) <= 2;
    }

    /// How many pixels of `frame` (classified with Nearest) are `colour`
    inline int CountClass(const Draw::Frame& classified, const Draw::Rgb& colour)
    {
        return classified.Count(colour);
    }

    /// The pixels of two classified frames that are `colour` in one and not the other
    inline int ClassDifferences(const Draw::Frame& a, const Draw::Frame& b, const Draw::Rgb& colour)
    {
        int differences = 0;
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                differences += ((a.At(x, y) == colour) != (b.At(x, y) == colour)) ? 1 : 0;
            }
        }
        return differences;
    }

    /// The face of two classified frames is the same set, to a few edge pixels. The software tests assert exact equality of
    /// the face pixels (alpha-tested pixels are exactly the face colour or not); on an antialiased frame a pixel at half
    /// coverage may fall either side of the classification, so the sets are compared with a small allowance: at most
    /// `allowancePercent` of the face's pixels or `minimum` pixels, whichever is more
    inline void ExpectSameFaceApprox(const Draw::Frame& a, const Draw::Frame& b, const Draw::Rgb& face,
                                     const double allowancePercent = 2.0, const int minimum = 6)
    {
        const int differences = ClassDifferences(a, b, face);
        const int allowed = std::max(minimum, static_cast<int>(a.Count(face) * allowancePercent / 100.0));
        EXPECT_LE(differences, allowed) << "the face pixels changed (" << differences << " of " << a.Count(face) << ")";
    }
}
