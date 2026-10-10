#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <text/FontAtlas.hpp>
#include <text/TextLayout.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"
#include "engine/ui/UIText.hpp"

// Multi-pass text effects: the pass list (Text::TextEffects::passes), how the shadow and outline settings
// map onto it (TextDrawing::ResolvePasses), and golden-image checks on the software renderer. The software
// text shader alpha-tests, so every pixel is exactly the background or one pass's colour, and the checks
// compare those pixel sets. The OpenGL path is not covered here (it can't run in CI).

using namespace N2Engine;
using Rendering::TextDrawing::ResolvedPasses;
using Rendering::TextDrawing::ResolvePasses;
using Renderer::Software::SoftwareRenderer;

namespace
{
    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

    constexpr Rgb Black{0, 0, 0};
    constexpr Rgb Face{255, 255, 0};  // Color::Yellow
    constexpr Rgb Blue{0, 0, 255};    // Color::Blue
    constexpr Rgb Red{255, 0, 0};     // Color::Red

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
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

    /// "Hi" in yellow, spaced out so no effect reaches the next glyph, drawn by a TextRenderer (depth-tested,
    /// as in a scene) through an orthographic camera that fits the layout with a margin for the effects
    class PassScene
    {
    public:
        static constexpr int kWidth = 160;
        static constexpr int kHeight = 96;
        static constexpr float kMargin = 20.0f;

        PassScene()
        {
            _initialized = _renderer.Initialize(nullptr, kWidth, kHeight);
            _renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            _object = GameObject::Create("Passes");
            _text = _object->AddComponent<Rendering::TextRenderer>();
            _text->SetText("Hi");
            _text->SetColor(Common::Color::Yellow);
            _text->SetLetterSpacing(0.4f);

            const Text::Rect bounds = _text->GetLayout().bounds;
            _scale = std::min((kWidth - 2.0f * kMargin) / bounds.Width(), (kHeight - 2.0f * kMargin) / bounds.Height());
            const float left = (bounds.minX + bounds.maxX) * 0.5f - kWidth * 0.5f / _scale;
            const float bottom = (bounds.minY + bounds.maxY) * 0.5f - kHeight * 0.5f / _scale;
            _camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
            _camera.SetOrthographic(left, left + kWidth / _scale, bottom, bottom + kHeight / _scale, 0.1f, 100.0f);
        }

        ~PassScene()
        {
            if (_text)
            {
                _text->CleanupRenderResources(&_renderer);
            }
            _renderer.Shutdown();
        }

        PassScene(const PassScene &) = delete;
        PassScene &operator=(const PassScene &) = delete;

        [[nodiscard]] bool Ok() const { return _initialized && _text != nullptr; }

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

    Text::TextPass Pass(const Common::Color &color, const float offsetX, const float offsetY, const float width = 0.0f,
                        const float softness = 0.0f, const int order = 0)
    {
        return Text::TextPass{color, Math::Vector2(offsetX, offsetY), width, softness, order};
    }

    /// Counts the warnings whose text has `needle` in it while it is alive
    class WarningCounter
    {
    public:
        explicit WarningCounter(std::string needle) : _needle(std::move(needle))
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn && message.find(_needle) != std::string_view::npos)
                {
                    ++count;
                }
            };
        }
        ~WarningCounter() { Logger::logEvent -= _id; }
        WarningCounter(const WarningCounter &) = delete;
        WarningCounter &operator=(const WarningCounter &) = delete;

        int count = 0;

    private:
        std::string _needle;
        std::size_t _id = 0;
    };

    Text::TextEffects WithPasses(const std::vector<Text::TextPass> &passes)
    {
        Text::TextEffects effects;
        effects.passes = passes;
        return effects;
    }

    /// Pixels that are neither the background nor one of the colours
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
}

// ============================================================================
// The pass list
// ============================================================================

TEST(TextPassesTest, NoEffectsGiveNoPasses)
{
    const Text::AtlasSettings settings;
    const ResolvedPasses none = ResolvePasses(Text::TextEffects{}, settings, 512, 256);
    EXPECT_TRUE(none.effectPasses.empty());
    EXPECT_EQ(none.faceSoftness, 0.0f);
    EXPECT_FALSE(none.clamped);
    EXPECT_FALSE(Text::TextEffects{}.HasPasses());
}

TEST(TextPassesTest, TheShadowAndOutlineSettingsAreTheFirstTwoPasses)
{
    const Text::AtlasSettings settings;
    Text::TextEffects effects;
    effects.outlineWidth = 0.04f;
    effects.outlineColor = Common::Color::Blue;
    effects.shadowOffset = Math::Vector2(0.03f, -0.02f);
    effects.shadowColor = Common::Color::Red;
    effects.shadowSoftness = 0.02f;
    effects.softness = 0.01f;

    const ResolvedPasses resolved = ResolvePasses(effects, settings, 512, 256);
    ASSERT_EQ(resolved.effectPasses.size(), 2u);
    const auto &shadow = resolved.effectPasses[0];
    const auto &outline = resolved.effectPasses[1];

    // Back to front: the shadow, which includes the outline's width, then the outline
    EXPECT_EQ(shadow.color.r, 1.0f);
    EXPECT_EQ(shadow.color.b, 0.0f);
    EXPECT_GT(shadow.width, 0.0f);
    EXPECT_FLOAT_EQ(shadow.width, outline.width);
    EXPECT_NE(shadow.offsetU, 0.0f);
    EXPECT_GT(shadow.offsetV, 0.0f) << "the shadow moved down, so towards a larger v";
    EXPECT_EQ(outline.color.b, 1.0f);
    EXPECT_EQ(outline.offsetU, 0.0f);
    EXPECT_EQ(outline.offsetV, 0.0f);
    EXPECT_GT(resolved.faceSoftness, 0.0f);
    EXPECT_FLOAT_EQ(outline.softness, resolved.faceSoftness);
}

TEST(TextPassesTest, ExtraPassesFollowInOrderAndInvisibleOnesAreSkipped)
{
    const Text::AtlasSettings settings;
    Text::TextEffects effects;
    effects.outlineWidth = 0.02f;
    effects.outlineColor = Common::Color::Blue;
    effects.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.05f), Pass(Common::Color{1.0f, 1.0f, 1.0f, 0.0f}, 0.0f, 0.0f),
                      Pass(Common::Color::Green, 0.03f, 0.0f)};
    EXPECT_TRUE(effects.HasPasses());

    const ResolvedPasses resolved = ResolvePasses(effects, settings, 512, 256);
    ASSERT_EQ(resolved.effectPasses.size(), 3u) << "the outline, then the two visible extra passes";
    EXPECT_EQ(resolved.effectPasses[0].color.b, 1.0f);
    EXPECT_EQ(resolved.effectPasses[1].color.r, 1.0f);
    EXPECT_EQ(resolved.effectPasses[1].color.g, 0.0f);
    EXPECT_GT(resolved.effectPasses[1].width, resolved.effectPasses[0].width);
    EXPECT_EQ(resolved.effectPasses[2].color.g, 1.0f);
    EXPECT_GT(resolved.effectPasses[2].offsetU, 0.0f);

    Text::TextEffects hidden;
    hidden.passes = {Pass(Common::Color{1.0f, 0.0f, 0.0f, 0.0f}, 0.1f, 0.1f, 0.1f)};
    EXPECT_FALSE(hidden.HasPasses());
    EXPECT_TRUE(ResolvePasses(hidden, settings, 512, 256).effectPasses.empty());
}

TEST(TextPassesTest, PassesAreClampedToTheSpreadAndOddLengthsCountAsZero)
{
    const Text::AtlasSettings settings;
    const float budget = Rendering::TextDrawing::MaxEffectEms(settings);
    const float valuePerEm = 0.5f * settings.basePx / static_cast<float>(settings.spreadPx);

    const ResolvedPasses big = ResolvePasses(WithPasses({Pass(Common::Color::Red, 5.0f, -5.0f, 5.0f, 5.0f)}), settings,
                                             512, 256);
    ASSERT_EQ(big.effectPasses.size(), 1u);
    EXPECT_TRUE(big.clamped);
    EXPECT_NEAR(big.effectPasses[0].width, budget * valuePerEm, 1e-5f) << "the width takes the whole spread";
    EXPECT_EQ(big.effectPasses[0].softness, 0.0f);
    EXPECT_EQ(big.effectPasses[0].offsetU, 0.0f);
    EXPECT_EQ(big.effectPasses[0].offsetV, 0.0f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ResolvedPasses odd = ResolvePasses(WithPasses({Pass(Common::Color::Red, nan, 0.0f, -1.0f, nan)}), settings,
                                             512, 256);
    ASSERT_EQ(odd.effectPasses.size(), 1u);
    EXPECT_FALSE(odd.clamped);
    EXPECT_EQ(odd.effectPasses[0].width, 0.0f);
    EXPECT_EQ(odd.effectPasses[0].softness, 0.0f);
    EXPECT_EQ(odd.effectPasses[0].offsetU, 0.0f);
}

TEST(TextPassesTest, TheComponentsAddAndClearPasses)
{
    auto object = GameObject::Create("PassSetters");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    EXPECT_TRUE(text->GetEffects().passes.empty());
    text->AddEffectPass(Pass(Common::Color::Red, 0.01f, 0.0f));
    text->AddEffectPass(Pass(Common::Color::Blue, 0.0f, 0.0f, 0.02f));
    ASSERT_EQ(text->GetEffects().passes.size(), 2u);
    EXPECT_EQ(text->GetEffects().passes[1].color.b, 1.0f);
    text->SetOutline(0.01f, Common::Color::Blue);
    text->ClearEffectPasses();
    EXPECT_TRUE(text->GetEffects().passes.empty());
    EXPECT_TRUE(text->GetEffects().HasOutline()) << "clearing the passes leaves the outline setting";

    auto uiObject = GameObject::Create("UIPassSetters");
    auto *ui = uiObject->AddComponent<UI::UIText>();
    ui->AddEffectPass(Pass(Common::Color::Red, 0.0f, 0.0f));
    EXPECT_EQ(ui->GetEffects().passes.size(), 1u);
    ui->ClearEffectPasses();
    EXPECT_TRUE(ui->GetEffects().passes.empty());
}

// ============================================================================
// Pixels on the software renderer
// ============================================================================

TEST(SoftwareTextPassesGoldenTest, TheShadowAndOutlineSettingsDrawAsTheirPassEquivalents)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    // The settings (one shader draw)...
    Text::TextEffects settings;
    settings.outlineWidth = 0.04f;
    settings.outlineColor = Common::Color::Blue;
    settings.shadowOffset = Math::Vector2(0.08f, -0.08f);
    settings.shadowColor = Common::Color::Red;
    const Frame fromSettings = scene.Render(settings);
    ASSERT_GT(fromSettings.Count(Red), 0);
    ASSERT_GT(fromSettings.Count(Blue), 0);

    // ...and the same list written out as passes (one draw each, then the face). The two agree here because
    // the glyphs are far enough apart that no glyph's effect reaches its neighbour's; where they do overlap
    // the single draw goes glyph by glyph and the pass list pass by pass, so the images differ there.
    const Frame fromPasses = scene.Render(
        WithPasses({Pass(Common::Color::Red, 0.08f, -0.08f, 0.04f), Pass(Common::Color::Blue, 0.0f, 0.0f, 0.04f)}));
    EXPECT_EQ(fromPasses.rgba, fromSettings.rgba) << "mapping the settings onto the pass list changed the image";
}

TEST(SoftwareTextPassesGoldenTest, LaterPassesDrawOverEarlierOnesAndTheFaceIsLast)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    const Text::TextPass red = Pass(Common::Color::Red, 0.06f, -0.06f, 0.03f);
    const Text::TextPass blue = Pass(Common::Color::Blue, 0.0f, 0.0f, 0.03f);
    const Frame redOnly = scene.Render(WithPasses({red}));
    const Frame blueOnly = scene.Render(WithPasses({blue}));
    const Frame redThenBlue = scene.Render(WithPasses({red, blue}));
    const Frame blueThenRed = scene.Render(WithPasses({blue, red}));

    EXPECT_EQ(OtherColours(redThenBlue, {Face, Red, Blue}), 0);
    ExpectSameFace(plain, redThenBlue);
    ExpectSameFace(plain, blueThenRed);

    // Where both shapes cover (and the face doesn't), the later pass wins
    int overlap = 0;
    int wrongForward = 0;
    int wrongReverse = 0;
    for (int y = 0; y < plain.height; ++y)
    {
        for (int x = 0; x < plain.width; ++x)
        {
            if (plain.At(x, y) == Face || redOnly.At(x, y) != Red || blueOnly.At(x, y) != Blue)
            {
                continue;
            }
            ++overlap;
            wrongForward += redThenBlue.At(x, y) == Blue ? 0 : 1;
            wrongReverse += blueThenRed.At(x, y) == Red ? 0 : 1;
        }
    }
    EXPECT_GT(overlap, 0) << "the two shapes never overlap, so the test shows nothing";
    EXPECT_EQ(wrongForward, 0) << "the second pass didn't draw over the first";
    EXPECT_EQ(wrongReverse, 0) << "reversing the passes didn't reverse which one is on top";
}

TEST(SoftwareTextPassesGoldenTest, ExtraPassesDrawAfterTheSettingsOutlineAndShadow)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    Text::TextEffects outlined;
    outlined.outlineWidth = 0.03f;
    outlined.outlineColor = Common::Color::Blue;
    const Frame outlineOnly = scene.Render(outlined);

    // A wider red pass after the blue outline setting
    Text::TextEffects both = outlined;
    both.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.06f)};
    const Frame withPass = scene.Render(both);
    EXPECT_EQ(OtherColours(withPass, {Face, Blue, Red}), 0);
    EXPECT_GT(withPass.Count(Red), 0);

    // The settings shadow and outline are the first entries of the list and the extra passes follow them,
    // so the red pass is drawn after the outline and covers it where it reaches
    int outlineLost = 0;
    for (int y = 0; y < withPass.height; ++y)
    {
        for (int x = 0; x < withPass.width; ++x)
        {
            outlineLost += (withPass.At(x, y) == Red && outlineOnly.At(x, y) == Blue) ? 1 : 0;
        }
    }
    EXPECT_GT(outlineLost, 0) << "extra passes are drawn after the settings outline, so they cover it";
}

TEST(SoftwareTextPassesGoldenTest, EffectPassesNeverCoverTheFace)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    ASSERT_GT(plain.Count(Face), 0);

    // A fat pass right on the text, a shadow-like one and a hidden one: the face pixels never change
    const Frame frame = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.1f),
                                                 Pass(Common::Color::Blue, 0.05f, 0.05f, 0.05f),
                                                 Pass(Common::Color{0.0f, 1.0f, 0.0f, 0.0f}, 0.0f, 0.0f)}));
    ExpectSameFace(plain, frame);
    EXPECT_EQ(OtherColours(frame, {Face, Red, Blue}), 0) << "the hidden pass drew";
    EXPECT_GT(frame.Count(Red), 0);
    EXPECT_GT(frame.Count(Blue), 0);
}

TEST(SoftwareTextPassesGoldenTest, APassWithoutOffsetOrWidthIsACopyOfTheFace)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    // Behind the face, a pass in the face's own shape is fully hidden by the face
    const Frame frame = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f)}));
    EXPECT_EQ(frame.rgba, plain.rgba);
}

TEST(SoftwareTextPassesGoldenTest, AWiderPassIsARingAroundTheFace)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    const Frame thin = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.02f)}));
    const Frame wide = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.06f)}));
    ExpectSameFace(plain, wide);
    EXPECT_GT(thin.Count(Red), 0);
    EXPECT_GT(wide.Count(Red), thin.Count(Red)) << "a wider pass covers more";
}

TEST(SoftwareTextPassesGoldenTest, PassesBeyondTheSpreadAreClampedNotDrawnAsBoxes)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    const Frame huge = scene.Render(WithPasses({Pass(Common::Color::Red, 3.0f, -3.0f, 3.0f, 3.0f)}));
    ExpectSameFace(plain, huge);
    EXPECT_EQ(OtherColours(huge, {Face, Red}), 0);
    // The clamp gives exactly the widest pass the font allows, not a box over the glyph quads
    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    const float widest = Rendering::TextDrawing::MaxEffectEms(font->GetSdfFont().GetAtlas().GetSettings());
    const Frame atMax = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, widest)}));
    EXPECT_EQ(huge.rgba, atMax.rgba);
}

// ============================================================================
// Lua
// ============================================================================

TEST(TextPassesTest, LuaAddsAndClearsPasses)
{
    using Scripting::LuaRuntime;
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    sol::state &lua = LuaRuntime::Instance().GetState();

    const auto run = [&lua](const std::string &code)
    {
        const sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error error = result;
            ADD_FAILURE() << "Lua error: " << error.what() << "\nin: " << code;
        }
    };

    for (const char *type : {"TextRenderer", "UIText"})
    {
        SCOPED_TRACE(type);
        lua["text_pass_type"] = std::string(type);
        run(R"(
            text_pass_go = GameObject.Create("LuaTextPasses")
            text_pass = text_pass_go:AddComponent(text_pass_type)
            assert(text_pass:GetEffectPassCount() == 0, "no passes by default")

            text_pass:AddEffectPass(0.02, -0.02, Color.new(1, 0, 0, 1), 0.03, 0.01)
            text_pass:AddEffectPass(0, 0, Color.new(0, 0, 1, 1))
            text_pass:AddEffectPass(0.01, 0, Color.new(0, 1, 0, 1), 0.02, 0, -250)
            text_pass_count = text_pass:GetEffectPassCount()

            text_pass:SetOutline(0.02, Color.new(0, 1, 0, 1))
            text_pass:ClearEffectPasses()
            text_pass_cleared = text_pass:GetEffectPassCount()
            local width = text_pass:GetOutline()
            text_pass_outline = width
        )");
        EXPECT_EQ(lua["text_pass_count"].get<int>(), 3);
        EXPECT_EQ(lua["text_pass_cleared"].get<int>(), 0);
        EXPECT_FLOAT_EQ(lua["text_pass_outline"].get<float>(), 0.02f) << "clearing the passes keeps the outline";
        run("text_pass_go = nil; text_pass = nil");
    }
}

// ============================================================================
// Order
// ============================================================================

TEST(TextPassesTest, TheSettingsHaveFixedOrdersAndExtraPassesAreStableSortedByOrder)
{
    EXPECT_EQ(Text::kShadowOrder, -200);
    EXPECT_EQ(Text::kOutlineOrder, -100);
    EXPECT_EQ(Text::kMaxPassOrder, 0);
    EXPECT_EQ(Text::TextPass{}.order, 0) << "the default order";

    const Text::AtlasSettings settings;
    Text::TextEffects effects;
    effects.outlineWidth = 0.02f;
    effects.outlineColor = Common::Color::Blue;
    effects.shadowOffset = Math::Vector2(0.03f, -0.03f);
    effects.shadowColor = Common::Color::Red;
    // Colours tell the entries apart: green (-300), white (-150), yellow (-100, ties with the outline and
    // was added later), cyan (0), magenta (0, added after cyan)
    effects.passes = {Pass(Common::Color::Cyan, 0.0f, 0.0f, 0.01f, 0.0f, 0),
                      Pass(Common::Color::Green, 0.0f, 0.0f, 0.01f, 0.0f, -300),
                      Pass(Common::Color::White, 0.0f, 0.0f, 0.01f, 0.0f, -150),
                      Pass(Common::Color::Yellow, 0.0f, 0.0f, 0.01f, 0.0f, -100),
                      Pass(Common::Color::Magenta, 0.0f, 0.0f, 0.01f, 0.0f, 0)};

    const ResolvedPasses resolved = ResolvePasses(effects, settings, 512, 256);
    ASSERT_EQ(resolved.effectPasses.size(), 7u);
    EXPECT_FALSE(resolved.orderClamped);
    const std::vector<Common::Color> expected = {Common::Color::Green,   Common::Color::Red,     Common::Color::White,
                                                 Common::Color::Blue,    Common::Color::Yellow,  Common::Color::Cyan,
                                                 Common::Color::Magenta};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const Common::Color &got = resolved.effectPasses[i].color;
        EXPECT_TRUE(got.r == expected[i].r && got.g == expected[i].g && got.b == expected[i].b) << "entry " << i;
    }
}

TEST(TextPassesTest, AnOrderAboveZeroCountsAsZero)
{
    const Text::AtlasSettings settings;
    Text::TextEffects high;
    high.outlineWidth = 0.02f;
    high.outlineColor = Common::Color::Blue;
    high.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.03f, 0.0f, 7), Pass(Common::Color::Green, 0.0f, 0.0f, 0.04f)};
    const ResolvedPasses resolved = ResolvePasses(high, settings, 512, 256);
    EXPECT_TRUE(resolved.orderClamped);

    Text::TextEffects zero = high;
    zero.passes[0].order = 0;
    const ResolvedPasses same = ResolvePasses(zero, settings, 512, 256);
    EXPECT_FALSE(same.orderClamped);
    ASSERT_EQ(resolved.effectPasses.size(), same.effectPasses.size());
    for (std::size_t i = 0; i < same.effectPasses.size(); ++i)
    {
        EXPECT_EQ(resolved.effectPasses[i].color.r, same.effectPasses[i].color.r) << i;
        EXPECT_EQ(resolved.effectPasses[i].color.g, same.effectPasses[i].color.g) << i;
        EXPECT_EQ(resolved.effectPasses[i].width, same.effectPasses[i].width) << i;
    }

    // A pass that doesn't draw doesn't count
    Text::TextEffects hidden;
    hidden.passes = {Pass(Common::Color{1.0f, 0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, 3)};
    EXPECT_FALSE(ResolvePasses(hidden, settings, 512, 256).orderClamped);
}

TEST(TextPassesTest, TheOrderAndSpreadWarningsNameTheirCause)
{
    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    const Text::TextLayout layout = font->Layout("Hi");
    const auto model = Math::Matrix<float, 4, 4>::identity();

    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, 32, 32));
    Rendering::TextDrawing::DrawResources resources;
    resources.Bind(&renderer);
    renderer.BeginFrame();

    // Component names no other test draws with, so these are the first warnings for them in the process
    constexpr std::string_view orderName = "TextPassOrderWarnTest";
    constexpr std::string_view spreadName = "TextPassSpreadWarnTest";
    WarningCounter orderWarnings{std::string(orderName)};
    WarningCounter spreadWarnings{std::string(spreadName)};
    WarningCounter namesPasses{"extra effect pass's width"};

    const Text::TextEffects high = WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.02f, 0.0f, 4)});
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(resources.Draw(font, layout, 1, model.Data(), Common::Color::White, high,
                                   Renderer::Common::RenderState::Transparent(), orderName));
    }
    EXPECT_EQ(orderWarnings.count, 1) << "one warning, however many draws";
    EXPECT_EQ(spreadWarnings.count, 0);

    // A text whose only clamped effect is an extra pass: the warning names the extra passes
    const Text::TextEffects wide = WithPasses({Pass(Common::Color::Red, 3.0f, 0.0f, 3.0f)});
    ASSERT_TRUE(resources.Draw(font, layout, 1, model.Data(), Common::Color::White, wide,
                               Renderer::Common::RenderState::Transparent(), spreadName));
    EXPECT_EQ(spreadWarnings.count, 1);
    EXPECT_EQ(namesPasses.count, 1) << "the clamp warning doesn't mention extra passes";

    renderer.EndFrame();
    resources.Release(true);
    renderer.Shutdown();
}

TEST(SoftwareTextPassesGoldenTest, APassBelowTheShadowSettingDrawsBehindIt)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    Text::TextEffects shadowOnly;
    shadowOnly.shadowOffset = Math::Vector2(0.04f, -0.04f);
    shadowOnly.shadowColor = Common::Color::Blue;
    const Frame shadowFrame = scene.Render(shadowOnly);
    const Frame redFrame = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.05f)}));

    const auto withRed = [&shadowOnly](const int order)
    {
        Text::TextEffects effects = shadowOnly;
        effects.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.05f, 0.0f, order)};
        return effects;
    };
    const Frame behind = scene.Render(withRed(-300));
    const Frame inFront = scene.Render(withRed(0));
    const Frame plain = scene.Render(Text::TextEffects{});
    ExpectSameFace(plain, behind);
    ExpectSameFace(plain, inFront);

    // Where the shadow and the red pass both cover (the face doesn't): the one drawn later is on top
    int overlap = 0;
    int wrongBehind = 0;
    int wrongFront = 0;
    for (int y = 0; y < plain.height; ++y)
    {
        for (int x = 0; x < plain.width; ++x)
        {
            if (plain.At(x, y) == Face || shadowFrame.At(x, y) != Blue || redFrame.At(x, y) != Red)
            {
                continue;
            }
            ++overlap;
            wrongBehind += behind.At(x, y) == Blue ? 0 : 1;
            wrongFront += inFront.At(x, y) == Red ? 0 : 1;
        }
    }
    EXPECT_GT(overlap, 0) << "the shapes never overlap, so the test shows nothing";
    EXPECT_EQ(wrongBehind, 0) << "an order below the shadow's didn't draw behind it";
    EXPECT_EQ(wrongFront, 0) << "the default order didn't draw in front of the shadow";
}

TEST(SoftwareTextPassesGoldenTest, APassBetweenTheShadowAndTheOutlineCoversOnlyTheShadow)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    constexpr Rgb Green{0, 255, 0};
    Text::TextEffects settings;
    settings.shadowOffset = Math::Vector2(0.04f, -0.04f);
    settings.shadowColor = Common::Color::Blue;
    settings.outlineWidth = 0.03f;
    settings.outlineColor = Common::Color::Green;
    const Frame base = scene.Render(settings);
    ASSERT_GT(base.Count(Blue), 0);
    ASSERT_GT(base.Count(Green), 0);

    Text::TextEffects mixed = settings;
    mixed.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.06f, 0.0f, -150)};
    const Frame frame = scene.Render(mixed);

    // The outline is drawn after the red pass, so every outline pixel of the base image is still there;
    // the red pass took over some of the shadow's
    int outlineLost = 0;
    int shadowCovered = 0;
    for (int y = 0; y < base.height; ++y)
    {
        for (int x = 0; x < base.width; ++x)
        {
            outlineLost += (base.At(x, y) == Green && frame.At(x, y) != Green) ? 1 : 0;
            shadowCovered += (base.At(x, y) == Blue && frame.At(x, y) == Red) ? 1 : 0;
        }
    }
    EXPECT_EQ(outlineLost, 0);
    EXPECT_GT(shadowCovered, 0);
}

TEST(SoftwareTextPassesGoldenTest, AnOrderAboveZeroDrawsLikeOrderZero)
{
    PassScene scene;
    ASSERT_TRUE(scene.Ok());

    const Text::TextPass atZero = Pass(Common::Color::Red, 0.0f, 0.0f, 0.04f, 0.0f, 0);
    Text::TextPass high = atZero;
    high.order = 9;
    const Frame zero = scene.Render(WithPasses({atZero}));
    const Frame above = scene.Render(WithPasses({high}));
    EXPECT_EQ(zero.rgba, above.rgba);
    EXPECT_GT(above.Count(Red), 0);
    EXPECT_GT(above.Count(Face), 0) << "the face is still drawn, and on top";
}

// ============================================================================
// Saving
// ============================================================================

namespace
{
    std::vector<Text::TextPass> SomePasses()
    {
        return {Pass(Common::Color{0.25f, 0.5f, 0.75f, 1.0f}, 0.03f, -0.02f, 0.04f, 0.01f, -300),
                Pass(Common::Color::Red, 0.0f, 0.0f, 0.02f, 0.0f, 0),
                Pass(Common::Color{0.0f, 1.0f, 0.0f, 0.5f}, -0.01f, 0.01f, 0.0f, 0.05f, -150)};
    }

    void ExpectSamePasses(const std::vector<Text::TextPass> &actual, const std::vector<Text::TextPass> &expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            SCOPED_TRACE(i);
            EXPECT_EQ(actual[i].color.r, expected[i].color.r);
            EXPECT_EQ(actual[i].color.g, expected[i].color.g);
            EXPECT_EQ(actual[i].color.b, expected[i].color.b);
            EXPECT_EQ(actual[i].color.a, expected[i].color.a);
            EXPECT_EQ(actual[i].offset.x, expected[i].offset.x);
            EXPECT_EQ(actual[i].offset.y, expected[i].offset.y);
            EXPECT_EQ(actual[i].width, expected[i].width);
            EXPECT_EQ(actual[i].softness, expected[i].softness);
            EXPECT_EQ(actual[i].order, expected[i].order);
        }
    }

    /// A scene with a TextRenderer and a UIText that both have SomePasses, the first also a shadow setting
    std::unique_ptr<Scene> SceneWithPasses(const std::string &name)
    {
        auto object = GameObject::Create("PassesSaved");
        auto *text = object->AddComponent<Rendering::TextRenderer>();
        text->SetText("Saved");
        text->SetShadow(Math::Vector2(0.03f, -0.03f), Common::Color::Red);
        for (const Text::TextPass &pass : SomePasses())
        {
            text->AddEffectPass(pass);
        }
        auto uiObject = GameObject::Create("PassesSavedUI");
        auto *ui = uiObject->AddComponent<UI::UIText>();
        for (const Text::TextPass &pass : SomePasses())
        {
            ui->AddEffectPass(pass);
        }

        auto scene = Scene::Create(name);
        scene->AddRootGameObject(object);
        scene->AddRootGameObject(uiObject);
        return scene;
    }

    void ExpectScenePasses(const std::unique_ptr<Scene> &scene)
    {
        ASSERT_NE(scene, nullptr);
        const auto object = scene->FindGameObject("PassesSaved");
        ASSERT_NE(object, nullptr);
        const auto *text = object->GetComponent<Rendering::TextRenderer>();
        ASSERT_NE(text, nullptr);
        ExpectSamePasses(text->GetEffects().passes, SomePasses());
        EXPECT_GT(text->GetEffects().shadowColor.a, 0.0f) << "the shadow setting is kept beside the passes";

        const auto uiObject = scene->FindGameObject("PassesSavedUI");
        ASSERT_NE(uiObject, nullptr);
        const auto *ui = uiObject->GetComponent<UI::UIText>();
        ASSERT_NE(ui, nullptr);
        ExpectSamePasses(ui->GetEffects().passes, SomePasses());
    }
}

TEST(TextPassesSaveTest, PassesSurviveASceneRoundTrip)
{
    const auto scene = SceneWithPasses("TextPasses_RoundTrip");
    const nlohmann::json saved = scene->Serialize();
    ExpectScenePasses(Scene::FromJSON(saved));

    // One key per component, an array of objects with every field
    const std::string dump = saved.dump();
    EXPECT_NE(dump.find("\"_effectPasses\""), std::string::npos) << dump;
    EXPECT_NE(dump.find("\"effectPasses\""), std::string::npos) << dump;
    EXPECT_NE(dump.find("\"order\":-300"), std::string::npos) << dump;
}

TEST(TextPassesSaveTest, APlaySnapshotKeepsThePasses)
{
    // The play snapshot is the scene's Serialize text (EditorServer::WritePlaySnapshot), loaded again with
    // Scene::FromJSON by the game that plays it
    const auto scene = SceneWithPasses("TextPasses_Snapshot");
    const std::string snapshotText = scene->Serialize().dump();
    const nlohmann::json snapshot = nlohmann::json::parse(snapshotText);
    ExpectScenePasses(Scene::FromJSON(snapshot));
}

TEST(TextPassesSaveTest, AComponentKeepsItsPassesWhenCopiedThroughItsJson)
{
    // Prefabs and duplicates are rebuilt from the object's JSON
    auto object = GameObject::Create("PassesCopied");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    for (const Text::TextPass &pass : SomePasses())
    {
        text->AddEffectPass(pass);
    }
    const nlohmann::json json = text->Serialize();

    auto copyObject = GameObject::Create("PassesCopy");
    auto *copy = copyObject->AddComponent<Rendering::TextRenderer>();
    copy->Deserialize(json);
    ExpectSamePasses(copy->GetEffects().passes, SomePasses());

    auto uiObject = GameObject::Create("PassesCopiedUI");
    auto *ui = uiObject->AddComponent<UI::UIText>();
    for (const Text::TextPass &pass : SomePasses())
    {
        ui->AddEffectPass(pass);
    }
    auto uiCopyObject = GameObject::Create("PassesCopyUI");
    auto *uiCopy = uiCopyObject->AddComponent<UI::UIText>();
    uiCopy->Deserialize(ui->Serialize());
    ExpectSamePasses(uiCopy->GetEffects().passes, SomePasses());
}

TEST(TextPassesSaveTest, ASceneSavedWithoutPassesLoadsWithNone)
{
    const auto scene = SceneWithPasses("TextPasses_Old");
    nlohmann::json old = scene->Serialize();
    ASSERT_NE(old.dump().find("effectPasses"), std::string::npos);

    // Remove the keys the way a scene saved before passes existed lacks them
    const std::function<void(nlohmann::json &)> strip = [&strip](nlohmann::json &node)
    {
        if (node.is_object())
        {
            node.erase("_effectPasses");
            node.erase("effectPasses");
            for (auto &entry : node.items())
            {
                strip(entry.value());
            }
        }
        else if (node.is_array())
        {
            for (auto &element : node)
            {
                strip(element);
            }
        }
    };
    strip(old);
    EXPECT_EQ(old.dump().find("ffectPasses"), std::string::npos);

    const auto loaded = Scene::FromJSON(old);
    ASSERT_NE(loaded, nullptr);
    const auto textObject = loaded->FindGameObject("PassesSaved");
    ASSERT_NE(textObject, nullptr);
    const auto *text = textObject->GetComponent<Rendering::TextRenderer>();
    ASSERT_NE(text, nullptr);
    EXPECT_TRUE(text->GetEffects().passes.empty());
    EXPECT_GT(text->GetEffects().shadowColor.a, 0.0f) << "the other effect settings still load";
    const auto uiObject = loaded->FindGameObject("PassesSavedUI");
    ASSERT_NE(uiObject, nullptr);
    const auto *ui = uiObject->GetComponent<UI::UIText>();
    ASSERT_NE(ui, nullptr);
    EXPECT_TRUE(ui->GetEffects().passes.empty());
}

TEST(TextPassesSaveTest, APassEntryMissingKeysLoadsDefaults)
{
    auto object = GameObject::Create("PassesPartial");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    nlohmann::json json = text->Serialize();
    json["_effectPasses"] = nlohmann::json::array({nlohmann::json{{"width", 0.05f}}, nlohmann::json::object()});
    text->Deserialize(json);

    const std::vector<Text::TextPass> &passes = text->GetEffects().passes;
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_EQ(passes[0].width, 0.05f);
    EXPECT_EQ(passes[0].order, 0);
    EXPECT_EQ(passes[0].offset.x, 0.0f);
    EXPECT_EQ(passes[0].color.a, 1.0f) << "the default colour is opaque black, so the pass draws";
    EXPECT_EQ(passes[1].width, 0.0f);
}
