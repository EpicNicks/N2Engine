#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <text/TextLayout.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/text/TextEffects.hpp"

// Golden-image tests of the text effects (outline, shadow) on the software renderer: a TextRenderer drawn
// headless and read back. The software text shader alpha-tests, so every pixel is exactly the background,
// the face colour, the outline colour or the shadow colour, and the checks compare those pixel sets.

using namespace N2Engine;
using Renderer::Software::SoftwareRenderer;

namespace
{
    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

    constexpr Rgb Black{0, 0, 0};
    constexpr Rgb Face{255, 255, 0};     // Color::Yellow
    constexpr Rgb Outline{0, 0, 255};    // Color::Blue
    constexpr Rgb Shadow{255, 0, 0};     // Color::Red

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        /// y counts up from the bottom row
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        [[nodiscard]] bool Inside(const int x, const int y) const
        {
            return x >= 0 && y >= 0 && x < width && y < height;
        }

        [[nodiscard]] int Count(const Rgb colour) const
        {
            int count = 0;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    count += At(x, y) == colour ? 1 : 0;
                }
            }
            return count;
        }
    };

    /// "Hi" in yellow, spaced out, drawn by a TextRenderer through an orthographic camera that fits the
    /// layout with a margin wide enough for the effects
    class EffectsScene
    {
    public:
        static constexpr int kWidth = 160;
        static constexpr int kHeight = 96;
        static constexpr float kMargin = 20.0f;

        EffectsScene()
        {
            _initialized = _renderer.Initialize(nullptr, kWidth, kHeight);
            _renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            _object = GameObject::Create("Effects");
            _text = _object->AddComponent<Rendering::TextRenderer>();
            _text->SetText("Hi");
            _text->SetColor(Common::Color::Yellow);
            // Glyphs further apart than any effect reaches: each glyph is one draw's quad, and an effect from
            // one glyph's quad may cover a neighbour's face where they come closer than that (docs/text.html)
            _text->SetLetterSpacing(0.4f);

            const Text::Rect bounds = _text->GetLayout().bounds;
            _scale = std::min((kWidth - 2.0f * kMargin) / bounds.Width(), (kHeight - 2.0f * kMargin) / bounds.Height());
            const float left = (bounds.minX + bounds.maxX) * 0.5f - kWidth * 0.5f / _scale;
            const float bottom = (bounds.minY + bounds.maxY) * 0.5f - kHeight * 0.5f / _scale;
            _camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
            _camera.SetOrthographic(left, left + kWidth / _scale, bottom, bottom + kHeight / _scale, 0.1f, 100.0f);
        }

        ~EffectsScene()
        {
            if (_text)
            {
                _text->CleanupRenderResources(&_renderer);
            }
            _renderer.Shutdown();
        }

        EffectsScene(const EffectsScene &) = delete;
        EffectsScene &operator=(const EffectsScene &) = delete;

        [[nodiscard]] bool Ok() const { return _initialized && _text != nullptr; }
        /// Pixels per em (the font size is 1 world unit)
        [[nodiscard]] float Scale() const { return _scale; }

        Frame Render(const Text::TextEffects &effects)
        {
            _text->SetEffects(effects);
            _renderer.BeginFrame();
            _renderer.SetViewProjection(_camera.GetViewMatrix().Data(), _camera.GetProjectionMatrix().Data());
            _text->Render(&_renderer);
            _renderer.EndFrame();
            _renderer.Present();
            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth) * kHeight * 4), kWidth, kHeight};
            _renderer.ReadFramebuffer(frame.rgba.data(), kWidth, kHeight);
            return frame;
        }

    private:
        SoftwareRenderer _renderer;
        bool _initialized = false;
        GameObject::Ptr _object;
        Rendering::TextRenderer *_text = nullptr;
        Camera _camera;
        float _scale = 1.0f;
    };

    /// Pixels in neither the background nor any of the given colours
    int OtherColours(const Frame &frame, const std::vector<Rgb> &allowed)
    {
        int other = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb pixel = frame.At(x, y);
                if (pixel != Black && std::ranges::find(allowed, pixel) == allowed.end())
                {
                    ++other;
                }
            }
        }
        return other;
    }

    /// Whether the face pixels (Face) of two frames are exactly the same set
    void ExpectSameFace(const Frame &a, const Frame &b)
    {
        int differences = 0;
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                if ((a.At(x, y) == Face) != (b.At(x, y) == Face))
                {
                    ++differences;
                }
            }
        }
        EXPECT_EQ(differences, 0) << "the face pixels changed";
    }

    /// Outline pixels with no face pixel within widthPx (rounded up) plus 2 pixels, in x and y
    int OutlinePixelsFartherThan(const Frame &frame, const float widthPx)
    {
        const int reach = static_cast<int>(std::ceil(widthPx)) + 2;
        int farPixels = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (frame.At(x, y) != Outline)
                {
                    continue;
                }
                bool nearFace = false;
                for (int oy = -reach; oy <= reach && !nearFace; ++oy)
                {
                    for (int ox = -reach; ox <= reach && !nearFace; ++ox)
                    {
                        nearFace = frame.Inside(x + ox, y + oy) && frame.At(x + ox, y + oy) == Face;
                    }
                }
                farPixels += nearFace ? 0 : 1;
            }
        }
        return farPixels;
    }

    Text::TextEffects WithOutline(const float width)
    {
        Text::TextEffects effects;
        effects.outlineWidth = width;
        effects.outlineColor = Common::Color::Blue;
        return effects;
    }
}

TEST(SoftwareTextEffectsGoldenTest, EffectsOffDrawsExactlyThePlainText)
{
    EffectsScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    ASSERT_GT(plain.Count(Face), 0) << "the text drew nothing";
    EXPECT_EQ(OtherColours(plain, {Face}), 0) << "plain text is only the face colour";

    // Effects that are off, however their other settings are set: an outline without width, a shadow with
    // a transparent colour, softness (which the software renderer ignores)
    Text::TextEffects off;
    off.outlineWidth = 0.0f;
    off.outlineColor = Common::Color::Blue;
    off.shadowOffset = Math::Vector2(0.05f, -0.05f);
    off.shadowColor = Common::Color{1.0f, 0.0f, 0.0f, 0.0f};
    off.shadowSoftness = 0.05f;
    off.softness = 0.05f;
    const Frame offFrame = scene.Render(off);
    EXPECT_EQ(offFrame.rgba, plain.rgba) << "effects that are off changed the image";
}

TEST(SoftwareTextEffectsGoldenTest, AnOutlineIsARingAroundTheFaceThatGrowsWithItsWidth)
{
    EffectsScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    const float narrowEms = 0.05f;
    const float wideEms = 0.12f;
    const Frame narrow = scene.Render(WithOutline(narrowEms));
    const Frame wide = scene.Render(WithOutline(wideEms));

    // Only face, outline and background; the face is exactly the plain text's
    EXPECT_EQ(OtherColours(narrow, {Face, Outline}), 0);
    EXPECT_EQ(OtherColours(wide, {Face, Outline}), 0);
    ExpectSameFace(plain, narrow);
    ExpectSameFace(plain, wide);

    // There is an outline, and a wider one covers more
    const int narrowCount = narrow.Count(Outline);
    const int wideCount = wide.Count(Outline);
    EXPECT_GT(narrowCount, 0) << "no outline drawn";
    EXPECT_GT(wideCount, narrowCount) << "a wider outline covers more pixels";

    // A ring: no face pixel touches the background (every face edge is outlined)...
    int faceNextToBackground = 0;
    for (int y = 0; y < narrow.height; ++y)
    {
        for (int x = 0; x < narrow.width; ++x)
        {
            if (narrow.At(x, y) != Face)
            {
                continue;
            }
            constexpr int dx[] = {1, -1, 0, 0};
            constexpr int dy[] = {0, 0, 1, -1};
            for (int i = 0; i < 4; ++i)
            {
                const int nx = x + dx[i];
                const int ny = y + dy[i];
                if (narrow.Inside(nx, ny) && narrow.At(nx, ny) == Black)
                {
                    ++faceNextToBackground;
                }
            }
        }
    }
    EXPECT_EQ(faceNextToBackground, 0) << "face pixels next to the background: the outline has gaps";

    // ...and no outline pixel is further from the face than the outline's width (plus rounding)
    EXPECT_EQ(OutlinePixelsFartherThan(narrow, narrowEms * scene.Scale()), 0);
    EXPECT_EQ(OutlinePixelsFartherThan(wide, wideEms * scene.Scale()), 0);
}

TEST(SoftwareTextEffectsGoldenTest, AShadowIsOffsetInItsDirectionAndStaysUnderTheFace)
{
    EffectsScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});

    // Down and to the right (+x right, +y up), by a whole number of pixels so moved pixels line up
    constexpr int shiftPx = 3;
    const float offsetEms = static_cast<float>(shiftPx) / scene.Scale();
    ASSERT_LT(offsetEms, 0.1f) << "the offset should be well inside the default font's limit";
    Text::TextEffects effects;
    effects.shadowOffset = Math::Vector2(offsetEms, -offsetEms);
    effects.shadowColor = Common::Color::Red;
    const Frame shadowed = scene.Render(effects);

    EXPECT_EQ(OtherColours(shadowed, {Face, Shadow}), 0);
    ExpectSameFace(plain, shadowed); // the shadow never overwrites the face
    ASSERT_GT(shadowed.Count(Shadow), 0) << "no shadow drawn";

    // The shadow is the face moved by the offset: almost every face pixel, moved, is face or shadow, and
    // the shadow reaches further right and further down than the face
    const int shiftX = shiftPx;
    const int shiftY = -shiftPx;
    int faceCount = 0;
    int movedCovered = 0;
    int faceMaxX = -1, faceMinY = shadowed.height, shadowMaxX = -1, shadowMinY = shadowed.height;
    for (int y = 0; y < shadowed.height; ++y)
    {
        for (int x = 0; x < shadowed.width; ++x)
        {
            const Rgb pixel = shadowed.At(x, y);
            if (pixel == Shadow)
            {
                shadowMaxX = std::max(shadowMaxX, x);
                shadowMinY = std::min(shadowMinY, y);
            }
            if (plain.At(x, y) != Face)
            {
                continue;
            }
            ++faceCount;
            faceMaxX = std::max(faceMaxX, x);
            faceMinY = std::min(faceMinY, y);
            const int mx = x + shiftX;
            const int my = y + shiftY;
            if (shadowed.Inside(mx, my) && (shadowed.At(mx, my) == Face || shadowed.At(mx, my) == Shadow))
            {
                ++movedCovered;
            }
        }
    }
    ASSERT_GT(faceCount, 0);
    EXPECT_GE(movedCovered, faceCount * 95 / 100) << "the shadow isn't the face moved by the offset";
    EXPECT_NEAR(shadowMaxX - faceMaxX, shiftX, 2) << "the shadow should reach right by the offset";
    EXPECT_NEAR(faceMinY - shadowMinY, -shiftY, 2) << "the shadow should reach down by the offset";

    // The opposite offset puts it on the other side
    effects.shadowOffset = Math::Vector2(-offsetEms, offsetEms);
    const Frame opposite = scene.Render(effects);
    int leftOfFace = 0;
    int rightOfFace = 0;
    int faceMinX = opposite.width;
    for (int y = 0; y < plain.height; ++y)
    {
        for (int x = 0; x < plain.width; ++x)
        {
            if (plain.At(x, y) == Face)
            {
                faceMinX = std::min(faceMinX, x);
            }
        }
    }
    for (int y = 0; y < opposite.height; ++y)
    {
        for (int x = 0; x < opposite.width; ++x)
        {
            if (opposite.At(x, y) == Shadow)
            {
                leftOfFace += x < faceMinX ? 1 : 0;
                rightOfFace += x > faceMaxX ? 1 : 0;
            }
        }
    }
    EXPECT_GT(leftOfFace, 0) << "a shadow moved left should show left of the text";
    EXPECT_EQ(rightOfFace, 0) << "a shadow moved left shouldn't show right of the text";
}

TEST(SoftwareTextEffectsGoldenTest, AnOutlinedShadowIsDrawnUnderTheOutline)
{
    EffectsScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    Text::TextEffects effects = WithOutline(0.04f);
    const Frame outlined = scene.Render(effects);
    effects.shadowOffset = Math::Vector2(0.08f, -0.08f);
    effects.shadowColor = Common::Color::Red;
    const Frame both = scene.Render(effects);

    EXPECT_EQ(OtherColours(both, {Face, Outline, Shadow}), 0);
    ExpectSameFace(plain, both);
    EXPECT_GT(both.Count(Shadow), 0);
    // The shadow goes under the outline: every outline pixel of the outlined text is still outline
    int outlineLost = 0;
    for (int y = 0; y < both.height; ++y)
    {
        for (int x = 0; x < both.width; ++x)
        {
            if (outlined.At(x, y) == Outline && both.At(x, y) != Outline)
            {
                ++outlineLost;
            }
        }
    }
    EXPECT_EQ(outlineLost, 0) << "the shadow overwrote outline pixels";
}

TEST(SoftwareTextEffectsGoldenTest, EffectsBeyondTheSpreadAreClampedNotDrawnAsBoxes)
{
    EffectsScene scene;
    ASSERT_TRUE(scene.Ok());

    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    const float maxEms = Rendering::TextDrawing::MaxEffectEms(font->GetAtlasSettings());
    ASSERT_GT(maxEms, 0.0f);

    // Far past the spread: drawn as the largest outline the spread allows, never filling the glyph quads
    const Frame atMax = scene.Render(WithOutline(maxEms));
    const Frame huge = scene.Render(WithOutline(10.0f));
    EXPECT_EQ(huge.rgba, atMax.rgba) << "an outline past the limit should draw as the limit";

    // A quad filled edge to edge would put outline pixels in the quad's corners, far from any face pixel
    EXPECT_EQ(OutlinePixelsFartherThan(huge, maxEms * scene.Scale()), 0)
        << "outline pixels far from the face: the glyph quads were filled";
}
