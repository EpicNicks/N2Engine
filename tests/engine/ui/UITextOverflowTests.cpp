#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <text/Utf8.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/text/Font.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

// Issue #445: UIText's overflow modes: what Truncate and Ellipsis keep, the map back to the original characters, Clip's
// pixels, IsOverflowing and the saved setting

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    const std::string Ellipsis = "\xE2\x80\xA6";

    class UITextOverflowTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            font = Text::Font::GetDefault();
            ASSERT_NE(font, nullptr);
            options.fontSize = 20.0f;
            // The default font (Latin subset) may or may not have U+2026
            mark = font->GetSdfFont().GetAtlas().FindCodepoint(U'…') ? Ellipsis : std::string("...");
        }

        [[nodiscard]] float WidthOf(const std::string &text) const
        {
            Text::LayoutOptions oneLine = options;
            oneLine.maxWidth = 0.0f;
            const Text::TextLayout layout = font->Layout(text, oneLine);
            return layout.lines.empty() ? 0.0f : layout.lines.front().width;
        }

        [[nodiscard]] float LineHeight() const { return font->Layout("x", options).bounds.Height(); }

        std::optional<UIText::FittedText> Fit(const std::string &text, const float width, const float height,
                                              const TextOverflow overflow) const
        {
            Text::LayoutOptions wrapped = options;
            wrapped.maxWidth = wrap ? width : 0.0f;
            return UIText::FitText(*font, text, wrapped, width, height, overflow);
        }

        std::shared_ptr<Text::Font> font;
        Text::LayoutOptions options;
        std::string mark;
        bool wrap = false;
    };
}

TEST_F(UITextOverflowTest, TextThatFitsIsLeftAlone)
{
    const float width = WidthOf("Hello");
    EXPECT_FALSE(Fit("Hello", width, LineHeight(), TextOverflow::Truncate).has_value()) << "exactly its size";
    EXPECT_FALSE(Fit("Hello", 500.0f, 100.0f, TextOverflow::Ellipsis).has_value());
    EXPECT_FALSE(Fit("Hello", 1.0f, 1.0f, TextOverflow::Overflow).has_value());
    EXPECT_FALSE(Fit("Hello", 1.0f, 1.0f, TextOverflow::Clip).has_value());
    EXPECT_FALSE(Fit("", 1.0f, 1.0f, TextOverflow::Ellipsis).has_value());
}

TEST_F(UITextOverflowTest, ALongLineIsCutAfterTheLastCharacterThatFits)
{
    const float width = WidthOf("Hello wor") + 0.5f;
    const auto truncated = Fit("Hello world", width, 100.0f, TextOverflow::Truncate);
    ASSERT_TRUE(truncated.has_value());
    EXPECT_EQ(truncated->text, "Hello wor");
    ASSERT_EQ(truncated->source.size(), 9u);
    EXPECT_EQ(truncated->source.back(), 8u);

    const auto marked = Fit("Hello world", width, 100.0f, TextOverflow::Ellipsis);
    ASSERT_TRUE(marked.has_value());
    ASSERT_TRUE(marked->text.ends_with(mark)) << marked->text;
    EXPECT_LE(WidthOf(marked->text), width + 0.1f);
    const std::string kept = marked->text.substr(0, marked->text.size() - mark.size());
    EXPECT_EQ(std::string("Hello world").substr(0, kept.size()), kept);
    EXPECT_NE(kept.back(), ' ') << "no space before the mark";
    // One more character would not fit
    EXPECT_GT(WidthOf(std::string("Hello world").substr(0, kept.size() + (kept == "Hello" ? 2 : 1)) + mark), width);
    EXPECT_EQ(marked->source.size(), Text::DecodeUtf8(marked->text).size());
    EXPECT_EQ(marked->source.back(), UIText::NoCharacter) << "the mark is no character of the original";
}

TEST_F(UITextOverflowTest, SpacesBeforeTheMarkAreDropped)
{
    // Room for "Hi " and the mark, not for the next letter
    const float width = WidthOf("Hi " + mark) + 0.5f;
    const auto marked = Fit("Hi WWWW", width, 100.0f, TextOverflow::Ellipsis);
    ASSERT_TRUE(marked.has_value());
    EXPECT_EQ(marked->text, "Hi" + mark);
}

TEST_F(UITextOverflowTest, LinesThatDoNotFitTheHeightAreLeftOut)
{
    const float lineHeight = font->Layout("x", options).lineHeight;
    const std::string text = "one\ntwo\nthree\nfour";
    const float twoLines = LineHeight() + lineHeight + 0.5f;

    const auto truncated = Fit(text, 500.0f, twoLines, TextOverflow::Truncate);
    ASSERT_TRUE(truncated.has_value());
    EXPECT_EQ(truncated->text, "one\ntwo");
    ASSERT_EQ(truncated->source.size(), 7u);
    EXPECT_EQ(truncated->source[3], UIText::NoCharacter) << "the break";
    EXPECT_EQ(truncated->source[4], 4u) << "'t' of two";

    const auto marked = Fit(text, 500.0f, twoLines, TextOverflow::Ellipsis);
    ASSERT_TRUE(marked.has_value());
    EXPECT_EQ(marked->text, "one\ntwo" + mark);

    const auto none = Fit(text, 500.0f, 2.0f, TextOverflow::Ellipsis);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->text.empty()) << "not even one line";
}

TEST_F(UITextOverflowTest, WrappedLinesKeepTheirBreaks)
{
    wrap = true;
    const std::string text = "alpha beta gamma delta epsilon";
    const float width = WidthOf("alpha beta") + 2.0f;
    const float lineHeight = font->Layout("x", options).lineHeight;
    const auto marked = Fit(text, width, LineHeight() + lineHeight + 0.5f, TextOverflow::Ellipsis);
    ASSERT_TRUE(marked.has_value());
    Text::LayoutOptions wrapped = options;
    wrapped.maxWidth = width;
    const Text::TextLayout layout = font->Layout(marked->text, wrapped);
    ASSERT_EQ(layout.lines.size(), 2u) << marked->text;
    EXPECT_TRUE(marked->text.ends_with(mark));
    EXPECT_TRUE(marked->text.starts_with("alpha beta")) << marked->text;
    for (const Text::LineInfo &line : layout.lines)
    {
        EXPECT_LE(line.width, width + 0.1f);
    }
    // The second line's characters map to where they are in the original
    const std::size_t second = std::string(text).find("gamma");
    ASSERT_NE(second, std::string::npos);
    const std::size_t breakAt = marked->text.find('\n');
    ASSERT_NE(breakAt, std::string::npos);
    EXPECT_EQ(marked->source[breakAt + 1], second);
}

TEST_F(UITextOverflowTest, AWidthTooSmallForTheMarkLeavesTheLineEmpty)
{
    const auto marked = Fit("Wide", 1.0f, 100.0f, TextOverflow::Ellipsis);
    ASSERT_TRUE(marked.has_value());
    EXPECT_TRUE(marked->text.empty());
    EXPECT_TRUE(marked->source.empty());
}

TEST_F(UITextOverflowTest, NonAsciiTextIsCutBetweenCharacters)
{
    const std::string text = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"; // six e-acute
    const auto truncated = Fit(text, WidthOf("\xC3\xA9\xC3\xA9\xC3\xA9") + 0.5f, 100.0f, TextOverflow::Truncate);
    ASSERT_TRUE(truncated.has_value());
    EXPECT_EQ(truncated->text, "\xC3\xA9\xC3\xA9\xC3\xA9");
    EXPECT_EQ(truncated->source, (std::vector<std::uint32_t>{0, 1, 2}));
}

TEST(UITextOverflowComponentTest, TheLayoutIsOfWhatIsDrawnAndFollowsTheRect)
{
    const auto element = UISystem::CreateText("Label", "Hello overflowing world");
    auto *text = element->GetComponent<UIText>();
    text->SetWrap(false);
    text->SetFontSize(20.0f);
    const Rect narrow{0.0f, 0.0f, 60.0f, 40.0f};
    const Rect wide{0.0f, 0.0f, 600.0f, 40.0f};
    const std::size_t all = text->GetLayout(narrow).quads.size();
    text->SetOverflow(TextOverflow::Truncate);
    EXPECT_LT(text->GetLayout(narrow).quads.size(), all);
    EXPECT_LE(text->GetBoundsIn(narrow).maxX, narrow.XMax() + 0.1f);
    EXPECT_EQ(text->GetLayout(wide).quads.size(), all) << "a wider rect shows it all";
    EXPECT_LT(text->GetLayout(narrow).quads.size(), all);
    text->SetText("Hi");
    EXPECT_EQ(text->GetLayout(narrow).quads.size(), 2u) << "a new text is fitted again";
}

TEST(UITextOverflowComponentTest, IsOverflowingTellsOfTheCurrentRect)
{
    const auto scene = Scene::Create("Overflow_IsOverflowing");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto element = UISystem::CreateText("Label", "Hello overflowing world");
    canvas->AddChild(element, false);
    auto *text = element->GetComponent<UIText>();
    text->SetWrap(false);
    element->GetComponent<RectTransform>()->SetSizeDelta(Vector2{40.0f, 100.0f});
    static_cast<void>(UISystem::CollectGraphics(*scene, Vector2i{800, 600}));
    EXPECT_TRUE(text->IsOverflowing());
    element->GetComponent<RectTransform>()->SetSizeDelta(Vector2{600.0f, 100.0f});
    static_cast<void>(UISystem::CollectGraphics(*scene, Vector2i{800, 600}));
    EXPECT_FALSE(text->IsOverflowing());
    element->GetComponent<RectTransform>()->SetSizeDelta(Vector2{600.0f, 5.0f});
    static_cast<void>(UISystem::CollectGraphics(*scene, Vector2i{800, 600}));
    EXPECT_TRUE(text->IsOverflowing()) << "too low";
}

TEST(UITextOverflowComponentTest, ClipDrawsNothingOutsideTheRect)
{
    constexpr int width = 80;
    constexpr int height = 60;
    const auto scene = Scene::Create("Overflow_Clip");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto element = UISystem::CreateText("Label", "WWWWWWWWWWWW\nWWWWWWWWWWWW\nWWWWWWWWWWWW");
    canvas->AddChild(element, false);
    auto *rectTransform = element->GetComponent<RectTransform>();
    rectTransform->SetSizeDelta(Vector2{40.0f, 30.0f}); // centred: x 20..60, y 15..45
    auto *text = element->GetComponent<UIText>();
    text->SetWrap(false);
    text->SetFontSize(16.0f);

    const auto countLit = [&](int &inside, int &outside)
    {
        Renderer::Software::SoftwareRenderer renderer;
        ASSERT_TRUE(renderer.Initialize(nullptr, width, height));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
        renderer.BeginFrame();
        UISystem::Render(*scene, &renderer, Vector2i{width, height});
        renderer.EndFrame();
        renderer.Present();
        renderer.ReadFramebuffer(rgba.data(), width, height);
        renderer.Shutdown();
        inside = 0;
        outside = 0;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (rgba[(static_cast<std::size_t>(y) * width + x) * 4] > 128)
                {
                    // The rect is symmetric about the screen's middle, so the row order does not matter
                    (x >= 19 && x <= 60 && y >= 14 && y <= 45 ? inside : outside) += 1;
                }
            }
        }
    };

    int inside = 0;
    int outside = 0;
    countLit(inside, outside);
    EXPECT_GT(outside, 0) << "by default the text overflows";
    text->SetOverflow(TextOverflow::Clip);
    countLit(inside, outside);
    EXPECT_GT(inside, 20);
    EXPECT_EQ(outside, 0);
}

TEST(UITextOverflowComponentTest, CharacterColorsStayOnTheirCharacters)
{
    const auto element = UISystem::CreateText("Label", "ab\ncd");
    auto *text = element->GetComponent<UIText>();
    text->SetFontSize(20.0f);
    text->SetOverflow(TextOverflow::Ellipsis);
    text->SetCharacterColor(3, Common::Color{1.0f, 0.0f, 0.0f, 1.0f}); // 'c'
    const Rect tall{0.0f, 0.0f, 400.0f, 100.0f};
    EXPECT_EQ(text->GetLayout(tall).quads.size(), 4u);
}

TEST(UITextOverflowComponentTest, ItIsSavedAndOldDataOverflows)
{
    const auto element = UISystem::CreateText("Label", "x");
    auto *text = element->GetComponent<UIText>();
    text->SetOverflow(TextOverflow::Ellipsis);
    nlohmann::json saved = text->Serialize();
    const auto other = UISystem::CreateText("Other", "y");
    auto *loaded = other->GetComponent<UIText>();
    loaded->Deserialize(saved);
    EXPECT_EQ(loaded->GetOverflow(), TextOverflow::Ellipsis);

    saved.erase("overflow");
    const auto old = UISystem::CreateText("Old", "z");
    old->GetComponent<UIText>()->Deserialize(saved);
    EXPECT_EQ(old->GetComponent<UIText>()->GetOverflow(), TextOverflow::Overflow);
}
