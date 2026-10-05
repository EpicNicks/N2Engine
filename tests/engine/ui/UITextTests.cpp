#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>
#include <text/FontAtlas.hpp>
#include <text/TextLayout.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/text/Font.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

// UIText through the UI pass and a renderer that keeps CPU-side resources (the software renderer's resource
// classes) and records every call. No window or GPU is involved.

using namespace N2Engine;
using Math::Vector2;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;
using Renderer::Common::TextureOptions;
using UI::Rect;
using UI::UISystem;
using UI::UIText;

namespace
{
    const Vector2i Viewport{800, 600};

    struct RecordedDraw
    {
        IMesh *mesh = nullptr;
        std::array<float, 16> model{};
        IMaterial *material = nullptr;
        RenderState state;
    };

    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        bool hasTextShader = true;

        std::vector<RecordedDraw> draws;
        int createdTextures = 0;
        int createMeshCalls = 0;
        int updateMeshCalls = 0;
        int destroyMeshCalls = 0;
        int destroyTextureCalls = 0;
        int createMaterialCalls = 0;
        int destroyMaterialCalls = 0;

        std::vector<std::unique_ptr<Renderer::Software::SWMesh>> meshes;
        std::vector<std::unique_ptr<Renderer::Software::SWTexture>> textures;
        std::vector<std::unique_ptr<Renderer::Software::SWMaterial>> materials;
        Renderer::Software::SWShader textShader{Renderer::Software::SWShaderType::Unlit};
        Renderer::Software::SWShader unlitShader{Renderer::Software::SWShaderType::Unlit};

        IMesh *CreateMesh(const MeshData &meshData) override
        {
            ++createMeshCalls;
            auto mesh = std::make_unique<Renderer::Software::SWMesh>();
            mesh->vertices = meshData.vertices;
            mesh->indices = meshData.indices;
            meshes.push_back(std::move(mesh));
            return meshes.back().get();
        }

        bool UpdateMesh(IMesh *mesh, const MeshData &meshData) override
        {
            ++updateMeshCalls;
            auto *sw = Find(meshes, mesh);
            if (!sw || meshData.vertices.empty())
            {
                return false;
            }
            sw->vertices = meshData.vertices;
            sw->indices = meshData.indices;
            return true;
        }

        void DestroyMesh(IMesh *mesh) override
        {
            ++destroyMeshCalls;
            Erase(meshes, mesh);
        }

        ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                const uint32_t channels) override
        {
            return CreateTexture(data, width, height, channels, TextureOptions::Default());
        }

        ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                const uint32_t channels, const TextureOptions &options) override
        {
            ++createdTextures;
            auto texture = std::make_unique<Renderer::Software::SWTexture>();
            texture->width = width;
            texture->height = height;
            texture->channels = channels;
            texture->options = options;
            texture->data.assign(data, data + static_cast<size_t>(width) * height * channels);
            textures.push_back(std::move(texture));
            return textures.back().get();
        }

        void DestroyTexture(ITexture *texture) override
        {
            ++destroyTextureCalls;
            Erase(textures, texture);
        }

        IMaterial *CreateMaterial(IShader *shader) override { return CreateMaterial(shader, nullptr); }

        IMaterial *CreateMaterial(IShader *shader, ITexture *texture) override
        {
            ++createMaterialCalls;
            materials.push_back(std::make_unique<Renderer::Software::SWMaterial>(shader, texture));
            return materials.back().get();
        }

        void DestroyMaterial(IMaterial *material) override
        {
            ++destroyMaterialCalls;
            Erase(materials, material);
        }

        [[nodiscard]] IShader *GetStandardTextShader() const override
        {
            return hasTextShader ? const_cast<Renderer::Software::SWShader *>(&textShader) : nullptr;
        }

        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material, const RenderState &state) override
        {
            RecordedDraw draw;
            draw.mesh = mesh;
            std::copy_n(modelMatrix, 16, draw.model.begin());
            draw.material = material;
            draw.state = state;
            draws.push_back(draw);
        }

        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(IShader *) override {}
        bool DestroyShaderProgram(IShader *) override { return false; }
        bool IsValidShader(IShader *) const override { return false; }
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] IShader *GetStandardUnlitShader() const override
        {
            return const_cast<Renderer::Software::SWShader *>(&unlitShader);
        }
        [[nodiscard]] IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Recording"; }

        [[nodiscard]] Renderer::Software::SWMesh *MeshOf(const RecordedDraw &draw) const
        {
            return Find(meshes, draw.mesh);
        }

        /// The draws that used the text shader
        [[nodiscard]] std::vector<RecordedDraw> TextDraws() const
        {
            std::vector<RecordedDraw> result;
            for (const RecordedDraw &draw : draws)
            {
                if (draw.material && draw.material->GetShader() == &textShader)
                {
                    result.push_back(draw);
                }
            }
            return result;
        }

    private:
        template <typename Owned, typename Base>
        static Owned *Find(const std::vector<std::unique_ptr<Owned>> &owned, Base *object)
        {
            const auto it = std::ranges::find_if(owned, [object](const auto &p) { return p.get() == object; });
            return it == owned.end() ? nullptr : it->get();
        }

        template <typename Owned, typename Base>
        static void Erase(std::vector<std::unique_ptr<Owned>> &owned, Base *object)
        {
            std::erase_if(owned, [object](const auto &p) { return p.get() == object; });
        }
    };

    /// A canvas in a scene with one text element whose rect is `rect` (canvas space)
    struct TextScene
    {
        std::unique_ptr<Scene> scene;
        GameObject::Ptr canvas;
        GameObject::Ptr element;
        UI::RectTransform *rectTransform = nullptr;
        UIText *text = nullptr;
    };

    void PlaceRect(UI::RectTransform &rectTransform, const Rect &rect)
    {
        // Pinned to the canvas's bottom-left corner, so the rect is exactly the given one
        rectTransform.SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform.SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform.SetPivot(Vector2{0.0f, 0.0f});
        rectTransform.SetAnchoredPosition(Vector2{rect.x, rect.y});
        rectTransform.SetSizeDelta(Vector2{rect.width, rect.height});
    }

    TextScene MakeTextScene(const std::string &name, const Rect &rect, const std::string &content)
    {
        TextScene result;
        result.scene = Scene::Create(name);
        result.canvas = UISystem::CreateCanvas(name + "_Canvas");
        result.scene->AddRootGameObject(result.canvas);
        result.element = UISystem::CreateText(name + "_Text", content);
        result.rectTransform = result.element->GetComponent<UI::RectTransform>();
        result.text = result.element->GetComponent<UIText>();
        PlaceRect(*result.rectTransform, rect);
        result.canvas->AddChild(result.element, false);
        return result;
    }

    std::shared_ptr<Text::Font> DefaultFont()
    {
        auto font = Text::Font::GetDefault();
        EXPECT_NE(font, nullptr) << "the embedded default font didn't build";
        return font;
    }

    /// How far a glyph's quad reaches past its ink: the SDF spread and atlas padding, in layout units,
    /// plus a pixel for rounding
    float QuadMargin(const float fontSize)
    {
        const Text::AtlasSettings &settings = DefaultFont()->GetAtlasSettings();
        return static_cast<float>(settings.spreadPx + settings.paddingPx + 1) / settings.basePx * fontSize;
    }

    struct Extent
    {
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
    };

    /// The drawn mesh's extent in canvas space (its vertices moved by the model matrix's translation)
    Extent CanvasExtent(const RecordingRenderer &renderer, const RecordedDraw &draw)
    {
        Extent extent;
        const Renderer::Software::SWMesh *mesh = renderer.MeshOf(draw);
        EXPECT_NE(mesh, nullptr);
        if (!mesh)
        {
            return extent;
        }
        for (const Renderer::Common::Vertex &vertex : mesh->vertices)
        {
            const float x = vertex.position[0] + draw.model[3];
            const float y = vertex.position[1] + draw.model[7];
            extent.minX = std::min(extent.minX, x);
            extent.maxX = std::max(extent.maxX, x);
            extent.minY = std::min(extent.minY, y);
            extent.maxY = std::max(extent.maxY, y);
        }
        return extent;
    }

    const char *Name(const Text::HorizontalAlign align)
    {
        switch (align)
        {
        case Text::HorizontalAlign::Left: return "Left";
        case Text::HorizontalAlign::Center: return "Center";
        case Text::HorizontalAlign::Right: return "Right";
        }
        return "?";
    }

    const char *Name(const Text::VerticalAlign align)
    {
        switch (align)
        {
        case Text::VerticalAlign::Top: return "Top";
        case Text::VerticalAlign::Middle: return "Middle";
        case Text::VerticalAlign::Bottom: return "Bottom";
        case Text::VerticalAlign::Baseline: return "Baseline";
        }
        return "?";
    }
}

// ============================================================================
// Settings, helpers and alignment
// ============================================================================

TEST(UITextTest, DefaultsAndTheCreateTextHelper)
{
    const auto element = UISystem::CreateText("Label", "Hello");
    EXPECT_EQ(element->GetName(), "Label");
    EXPECT_EQ(element->GetLayer(), Layers::UI);
    EXPECT_NE(element->GetComponent<UI::RectTransform>(), nullptr);
    const UIText *text = element->GetComponent<UIText>();
    ASSERT_NE(text, nullptr);

    EXPECT_EQ(text->GetTypeName(), "UIText");
    EXPECT_EQ(text->GetText(), "Hello");
    EXPECT_EQ(text->GetFont(), nullptr);
    EXPECT_EQ(text->GetEffectiveFont(), Text::Font::GetDefault());
    EXPECT_FLOAT_EQ(text->GetFontSize(), 24.0f);
    EXPECT_EQ(text->GetHorizontalAlign(), Text::HorizontalAlign::Left);
    EXPECT_EQ(text->GetVerticalAlign(), Text::VerticalAlign::Top);
    EXPECT_TRUE(text->GetWrap());
    EXPECT_FLOAT_EQ(text->GetLineSpacing(), 1.0f);
    EXPECT_FLOAT_EQ(text->GetLetterSpacing(), 0.0f);
    EXPECT_FALSE(text->GetRaycastTarget()) << "a label lets the pointer through by default";
    EXPECT_FLOAT_EQ(text->GetColor().r, Common::Color::White.r);
    EXPECT_FLOAT_EQ(text->GetColor().a, 1.0f);

    EXPECT_EQ(UISystem::CreateText()->GetName(), "Text");
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("UIText"));
}

TEST(UITextTest, AnchorsAreTheRectsEdgesAndCentres)
{
    constexpr Rect rect{100.0f, 50.0f, 300.0f, 120.0f};
    using H = Text::HorizontalAlign;
    using V = Text::VerticalAlign;

    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Left, V::Top).x, 100.0f);
    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Center, V::Top).x, 250.0f);
    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Right, V::Top).x, 400.0f);

    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Left, V::Top).y, 170.0f);
    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Left, V::Middle).y, 110.0f);
    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Left, V::Bottom).y, 50.0f);
    EXPECT_FLOAT_EQ(UIText::AnchorFor(rect, H::Left, V::Baseline).y, 110.0f) << "the first baseline on the centre";

    const auto model = UIText::ModelMatrixFor(Vector2{12.0f, 34.0f});
    EXPECT_FLOAT_EQ(model.Data()[3], 12.0f);
    EXPECT_FLOAT_EQ(model.Data()[7], 34.0f);
    EXPECT_FLOAT_EQ(model.Data()[11], 0.0f);
    EXPECT_FLOAT_EQ(model.Data()[0], 1.0f);
    EXPECT_FLOAT_EQ(model.Data()[5], 1.0f);
    EXPECT_FLOAT_EQ(model.Data()[10], 1.0f);
    EXPECT_FLOAT_EQ(model.Data()[15], 1.0f);
}

TEST(UITextTest, TheMeshLiesInsideTheRectForEveryAlignment)
{
    constexpr Rect rect{100.0f, 50.0f, 300.0f, 120.0f};
    constexpr float fontSize = 24.0f;
    const float margin = QuadMargin(fontSize);
    constexpr float eps = 1e-3f;

    for (const auto horizontal : {Text::HorizontalAlign::Left, Text::HorizontalAlign::Center,
                                  Text::HorizontalAlign::Right})
    {
        for (const auto vertical : {Text::VerticalAlign::Top, Text::VerticalAlign::Middle,
                                    Text::VerticalAlign::Bottom, Text::VerticalAlign::Baseline})
        {
            const std::string what = std::string(Name(horizontal)) + "/" + Name(vertical);
            RecordingRenderer renderer;
            TextScene s = MakeTextScene("UIText_Align", rect, "Hello, UI");
            s.text->SetFontSize(fontSize);
            s.text->SetHorizontalAlign(horizontal);
            s.text->SetVerticalAlign(vertical);

            UISystem::Render(*s.scene, &renderer, Viewport);
            const std::vector<RecordedDraw> draws = renderer.TextDraws();
            ASSERT_EQ(draws.size(), 1u) << what;
            EXPECT_EQ(draws[0].state, UISystem::OverlayState()) << what;

            // The block (advance widths, ascent to descent) is inside the rect and aligned as asked
            const Text::Rect bounds = s.text->GetBoundsIn(rect);
            EXPECT_GE(bounds.minX, rect.XMin() - eps) << what;
            EXPECT_LE(bounds.maxX, rect.XMax() + eps) << what;
            EXPECT_GE(bounds.minY, rect.YMin() - eps) << what;
            EXPECT_LE(bounds.maxY, rect.YMax() + eps) << what;
            switch (horizontal)
            {
            case Text::HorizontalAlign::Left:
                EXPECT_NEAR(bounds.minX, rect.XMin(), eps) << what;
                break;
            case Text::HorizontalAlign::Center:
                EXPECT_NEAR((bounds.minX + bounds.maxX) * 0.5f, rect.x + rect.width * 0.5f, eps) << what;
                break;
            case Text::HorizontalAlign::Right:
                EXPECT_NEAR(bounds.maxX, rect.XMax(), eps) << what;
                break;
            }
            switch (vertical)
            {
            case Text::VerticalAlign::Top:
                EXPECT_NEAR(bounds.maxY, rect.YMax(), eps) << what;
                break;
            case Text::VerticalAlign::Middle:
                EXPECT_NEAR((bounds.minY + bounds.maxY) * 0.5f, rect.y + rect.height * 0.5f, eps) << what;
                break;
            case Text::VerticalAlign::Bottom:
                EXPECT_NEAR(bounds.minY, rect.YMin(), eps) << what;
                break;
            case Text::VerticalAlign::Baseline:
            {
                const Text::TextLayout &layout = s.text->GetLayout(rect);
                ASSERT_FALSE(layout.lines.empty());
                EXPECT_NEAR(layout.lines[0].baseline + s.text->GetAnchor(rect).y, rect.y + rect.height * 0.5f, eps)
                    << what;
                break;
            }
            }

            // The drawn quads stay within the rect, give or take their SDF margin
            const Extent extent = CanvasExtent(renderer, draws[0]);
            EXPECT_GE(extent.minX, rect.XMin() - margin) << what;
            EXPECT_LE(extent.maxX, rect.XMax() + margin) << what;
            EXPECT_GE(extent.minY, rect.YMin() - margin) << what;
            EXPECT_LE(extent.maxY, rect.YMax() + margin) << what;

            // The mesh is the layout's, relative to the anchor the model matrix moves it to
            const Vector2 anchor = s.text->GetAnchor(rect);
            EXPECT_FLOAT_EQ(draws[0].model[3], anchor.x) << what;
            EXPECT_FLOAT_EQ(draws[0].model[7], anchor.y) << what;
            const MeshData expected = Rendering::TextDrawing::BuildMesh(s.text->GetLayout(rect));
            EXPECT_EQ(renderer.MeshOf(draws[0])->vertices.size(), expected.vertices.size()) << what;

            s.text->OnDestroy();
        }
    }
}

TEST(UITextTest, WrapsAtTheRectsWidth)
{
    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    constexpr Rect narrow{0.0f, 0.0f, 150.0f, 200.0f};
    constexpr Rect wide{0.0f, 0.0f, 2000.0f, 200.0f};

    TextScene s = MakeTextScene("UIText_Wrap", narrow, sentence);
    const Text::TextLayout &wrapped = s.text->GetLayout(narrow);
    EXPECT_GT(wrapped.lines.size(), 2u);
    for (const Text::LineInfo &line : wrapped.lines)
    {
        EXPECT_LE(line.width, narrow.width + 1e-3f);
    }
    EXPECT_FLOAT_EQ(s.text->GetLayoutOptions(narrow.width).maxWidth, 150.0f);

    // A rect wide enough keeps one line
    EXPECT_EQ(s.text->GetLayout(wide).lines.size(), 1u);

    // Without wrap, the line runs past the narrow rect
    s.text->SetWrap(false);
    EXPECT_FLOAT_EQ(s.text->GetLayoutOptions(narrow.width).maxWidth, 0.0f);
    const Text::TextLayout &unwrapped = s.text->GetLayout(narrow);
    ASSERT_EQ(unwrapped.lines.size(), 1u);
    EXPECT_GT(unwrapped.lines[0].width, narrow.width);
}

// ============================================================================
// Rebuilding
// ============================================================================

TEST(UITextTest, NothingIsRebuiltWhenNothingChanged)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Dirty", Rect{10.0f, 10.0f, 200.0f, 50.0f}, "Score: 0");

    for (int frame = 0; frame < 5; ++frame)
    {
        UISystem::Render(*s.scene, &renderer, Viewport);
    }
    EXPECT_EQ(renderer.TextDraws().size(), 5u);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 0);
    EXPECT_EQ(renderer.createdTextures, 1);
    EXPECT_EQ(renderer.createMaterialCalls, 1);

    // The same values again, and a new colour (a uniform), change nothing
    s.text->SetText("Score: 0");
    s.text->SetFontSize(s.text->GetFontSize());
    s.text->SetColor(Common::Color::Red);
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 0);

    const auto *material = dynamic_cast<const Renderer::Software::SWMaterial *>(renderer.draws.back().material);
    ASSERT_NE(material, nullptr);
    const std::array<float, 4> albedo = material->GetVec4("uAlbedo", {0, 0, 0, 0});
    EXPECT_FLOAT_EQ(albedo[0], 1.0f);
    EXPECT_FLOAT_EQ(albedo[1], 0.0f);
    EXPECT_FLOAT_EQ(albedo[3], 1.0f);

    s.text->OnDestroy();
}

TEST(UITextTest, EverySettingUpdatesTheMeshInPlace)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Settings", Rect{10.0f, 10.0f, 300.0f, 100.0f}, "Score: 0");
    UIText &text = *s.text;
    UISystem::Render(*s.scene, &renderer, Viewport);
    IMesh *const mesh = renderer.draws.back().mesh;

    int expectedUpdates = 0;
    const auto expectUpdate = [&](const char *what)
    {
        UISystem::Render(*s.scene, &renderer, Viewport);
        ++expectedUpdates;
        EXPECT_EQ(renderer.updateMeshCalls, expectedUpdates) << what;
    };
    text.SetText("Score: 100");
    expectUpdate("text");
    text.SetFontSize(30.0f);
    expectUpdate("font size");
    text.SetHorizontalAlign(Text::HorizontalAlign::Right);
    expectUpdate("horizontal alignment");
    text.SetVerticalAlign(Text::VerticalAlign::Bottom);
    expectUpdate("vertical alignment");
    text.SetLineSpacing(2.0f);
    expectUpdate("line spacing");
    text.SetLetterSpacing(0.2f);
    expectUpdate("letter spacing");
    text.SetWrap(false);
    expectUpdate("wrap");
    text.SetFont(Text::Font::GetDefault());
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(renderer.updateMeshCalls, expectedUpdates) << "the default font set explicitly is the same font";

    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.draws.back().mesh, mesh) << "the same mesh, updated";
    text.OnDestroy();
}

TEST(UITextTest, AMovedRectChangesOnlyTheModelMatrix)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Move", Rect{10.0f, 20.0f, 200.0f, 50.0f}, "Moving");
    UISystem::Render(*s.scene, &renderer, Viewport);
    const RecordedDraw before = renderer.draws.back();

    s.rectTransform->SetAnchoredPosition(Vector2{110.0f, 70.0f});
    UISystem::Render(*s.scene, &renderer, Viewport);
    const RecordedDraw after = renderer.draws.back();

    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 0) << "a move doesn't rebuild the mesh";
    EXPECT_EQ(after.mesh, before.mesh);
    EXPECT_FLOAT_EQ(after.model[3] - before.model[3], 100.0f);
    EXPECT_FLOAT_EQ(after.model[7] - before.model[7], 50.0f);
    s.text->OnDestroy();
}

TEST(UITextTest, AResizedRectRebuildsWhenTheWrapWidthChanges)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Resize", Rect{10.0f, 20.0f, 200.0f, 50.0f}, "Resizing text");
    UISystem::Render(*s.scene, &renderer, Viewport);

    // A new width is a new wrap width
    s.rectTransform->SetSizeDelta(Vector2{120.0f, 50.0f});
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(renderer.updateMeshCalls, 1);

    // A new height only moves the anchor (top-aligned: the top edge)
    const float topBefore = renderer.draws.back().model[7];
    s.rectTransform->SetSizeDelta(Vector2{120.0f, 80.0f});
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(renderer.updateMeshCalls, 1);
    EXPECT_FLOAT_EQ(renderer.draws.back().model[7] - topBefore, 30.0f);

    // Without wrap the width isn't an input
    s.text->SetWrap(false);
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(renderer.updateMeshCalls, 2) << "turning wrap off is a change";
    s.rectTransform->SetSizeDelta(Vector2{300.0f, 80.0f});
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(renderer.updateMeshCalls, 2);

    EXPECT_EQ(renderer.createMeshCalls, 1);
    s.text->OnDestroy();
}

TEST(UITextTest, EmptyTextAndNoTextShaderDrawNothing)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Empty", Rect{0.0f, 0.0f, 100.0f, 40.0f}, "");
    UISystem::Render(*s.scene, &renderer, Viewport);
    s.text->SetText("   ");
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_TRUE(renderer.draws.empty());
    EXPECT_EQ(renderer.createMeshCalls, 0);

    RecordingRenderer noShader;
    noShader.hasTextShader = false;
    s.text->SetText("Hidden");
    UISystem::Render(*s.scene, &noShader, Viewport);
    EXPECT_TRUE(noShader.draws.empty());
    EXPECT_EQ(noShader.createdTextures, 0);
    EXPECT_EQ(noShader.createMeshCalls, 0);
    EXPECT_EQ(noShader.createMaterialCalls, 0);

    // A null renderer or a rect without area is a no-op
    EXPECT_NO_THROW(s.text->RenderUI(nullptr, Rect{0.0f, 0.0f, 10.0f, 10.0f}, UISystem::OverlayState()));
    s.text->RenderUI(&renderer, Rect{0.0f, 0.0f, 0.0f, 10.0f}, UISystem::OverlayState());
    EXPECT_TRUE(renderer.draws.empty());
    s.text->OnDestroy();
}

// ============================================================================
// Resources
// ============================================================================

TEST(UITextTest, TheAtlasIsSharedWithTextRendererPerRendererAndFont)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Atlas", Rect{0.0f, 0.0f, 200.0f, 40.0f}, "UI");
    auto world = GameObject::Create("UIText_AtlasWorld");
    auto *worldText = world->AddComponent<Rendering::TextRenderer>();
    worldText->SetText("World");

    worldText->Render(&renderer);
    UISystem::Render(*s.scene, &renderer, Viewport);
    ASSERT_EQ(renderer.draws.size(), 2u);
    EXPECT_EQ(renderer.createdTextures, 1) << "both use the default font: one atlas texture";
    EXPECT_EQ(renderer.textures[0]->options, TextureOptions::SdfAtlas());
    EXPECT_EQ(renderer.draws[0].material->GetTexture(), renderer.draws[1].material->GetTexture());
    EXPECT_NE(renderer.draws[0].material, renderer.draws[1].material) << "each has its own colour";

    // The texture goes when its last user releases it, whichever kind
    worldText->CleanupRenderResources(&renderer);
    EXPECT_EQ(renderer.destroyTextureCalls, 0);
    s.text->OnDestroy();
    EXPECT_EQ(renderer.destroyTextureCalls, 1);
    EXPECT_TRUE(renderer.textures.empty());
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());

    // A second renderer gets its own atlas
    RecordingRenderer other;
    UISystem::Render(*s.scene, &other, Viewport);
    UISystem::Render(*s.scene, &renderer, Viewport);
    EXPECT_EQ(other.createdTextures, 1);
    EXPECT_TRUE(other.textures.empty()) << "moving to the other renderer released this one's atlas";
    EXPECT_EQ(renderer.createdTextures, 2);
    s.text->OnDestroy();
}

TEST(UITextTest, DestroyingTheObjectInASceneReleasesItsResources)
{
    RecordingRenderer renderer;
    TextScene s = MakeTextScene("UIText_Destroy", Rect{0.0f, 0.0f, 200.0f, 40.0f}, "Bye");
    s.scene->ProcessAttachQueue(); // as a loaded scene does
    UISystem::Render(*s.scene, &renderer, Viewport);
    ASSERT_EQ(renderer.meshes.size(), 1u);
    ASSERT_EQ(renderer.textures.size(), 1u);

    s.scene->Clear();

    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
    EXPECT_TRUE(renderer.textures.empty());
}

TEST(UITextTest, ARendererRecreatedAtTheSameAddressGetsNewResources)
{
    // The window re-creates its renderer, and the new one can land where the old one was
    std::optional<RecordingRenderer> renderer;
    renderer.emplace();
    TextScene s = MakeTextScene("UIText_Recreated", Rect{0.0f, 0.0f, 200.0f, 40.0f}, "Text");
    UISystem::Render(*s.scene, &*renderer, Viewport);
    ASSERT_EQ(renderer->createdTextures, 1);

    renderer.reset();   // frees everything it made
    renderer.emplace(); // same storage, so the same address

    UISystem::Render(*s.scene, &*renderer, Viewport);
    EXPECT_EQ(renderer->createdTextures, 1); // a new atlas, not the old renderer's
    EXPECT_EQ(renderer->meshes.size(), 1u);
    EXPECT_EQ(renderer->materials.size(), 1u);
    ASSERT_EQ(renderer->draws.size(), 1u);
    EXPECT_EQ(renderer->draws[0].mesh, renderer->meshes[0].get());
    EXPECT_EQ(renderer->destroyMeshCalls, 0); // nothing of the old renderer's is destroyed here
    EXPECT_EQ(renderer->destroyTextureCalls, 0);

    s.text->OnDestroy();
    EXPECT_TRUE(renderer->meshes.empty());
    EXPECT_TRUE(renderer->textures.empty());
}

// ============================================================================
// Hit testing and serialization
// ============================================================================

TEST(UITextTest, TextIsNotARaycastTargetUnlessTurnedOn)
{
    TextScene s = MakeTextScene("UIText_Hit", Rect{100.0f, 100.0f, 200.0f, 50.0f}, "Label");
    const Vector2 inside{150.0f, 120.0f};
    EXPECT_EQ(UISystem::HitTest(*s.scene, inside, Viewport), nullptr) << "the pointer goes through a label";

    s.text->SetRaycastTarget(true);
    EXPECT_EQ(UISystem::HitTest(*s.scene, inside, Viewport), s.element.get());
}

TEST(UITextTest, SettingsSurviveASceneRoundTrip)
{
    TextScene s = MakeTextScene("UIText_RoundTrip", Rect{5.0f, 6.0f, 70.0f, 80.0f}, "Saved text\nline two");
    UIText &text = *s.text;
    text.SetFontSize(18.5f);
    text.SetColor(Common::Color::Magenta);
    text.SetHorizontalAlign(Text::HorizontalAlign::Right);
    text.SetVerticalAlign(Text::VerticalAlign::Baseline);
    text.SetWrap(false);
    text.SetLineSpacing(1.25f);
    text.SetLetterSpacing(-0.05f);

    const nlohmann::json saved = s.scene->Serialize();
    const auto loadedScene = Scene::FromJSON(saved);
    ASSERT_NE(loadedScene, nullptr);
    const auto loadedObject = loadedScene->FindGameObject("UIText_RoundTrip_Text");
    ASSERT_NE(loadedObject, nullptr);
    const auto *loaded = loadedObject->GetComponent<UIText>();
    ASSERT_NE(loaded, nullptr) << "UIText wasn't registered, so loading dropped it";

    EXPECT_EQ(loaded->GetText(), "Saved text\nline two");
    EXPECT_EQ(loaded->GetFont(), nullptr);
    EXPECT_FLOAT_EQ(loaded->GetFontSize(), 18.5f);
    EXPECT_FLOAT_EQ(loaded->GetColor().r, Common::Color::Magenta.r);
    EXPECT_FLOAT_EQ(loaded->GetColor().g, Common::Color::Magenta.g);
    EXPECT_FLOAT_EQ(loaded->GetColor().b, Common::Color::Magenta.b);
    EXPECT_EQ(loaded->GetHorizontalAlign(), Text::HorizontalAlign::Right);
    EXPECT_EQ(loaded->GetVerticalAlign(), Text::VerticalAlign::Baseline);
    EXPECT_FALSE(loaded->GetWrap());
    EXPECT_FLOAT_EQ(loaded->GetLineSpacing(), 1.25f);
    EXPECT_FLOAT_EQ(loaded->GetLetterSpacing(), -0.05f);
    EXPECT_FALSE(loaded->GetRaycastTarget());

    // Alignments are saved by name, and the type by its name
    const std::string dump = saved.dump();
    EXPECT_NE(dump.find("\"Baseline\""), std::string::npos) << dump;
    EXPECT_NE(dump.find("\"Right\""), std::string::npos) << dump;
    EXPECT_NE(dump.find("\"UIText\""), std::string::npos) << dump;

    // A raycast target turned on is saved too
    auto copy = GameObject::Create("UIText_Copy");
    text.SetRaycastTarget(true);
    auto *copied = copy->AddComponent<UIText>();
    copied->Deserialize(text.Serialize());
    EXPECT_TRUE(copied->GetRaycastTarget());
    EXPECT_EQ(copied->GetText(), "Saved text\nline two");
}
