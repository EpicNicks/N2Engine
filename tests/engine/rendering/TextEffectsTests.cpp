#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Matrix.hpp>
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

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/UIText.hpp"

// Text effects (outline, shadow, softness) on TextRenderer and UIText: the settings, how they become the text
// material's uniforms (TextDrawing::ResolveEffects), the clamping to the font's SDF spread, saving and Lua.
// The pixels are checked in SoftwareTextEffectsGoldenTests.cpp.

using namespace N2Engine;
using Rendering::TextDrawing::EffectUniforms;
using Rendering::TextDrawing::ResolveEffects;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;
using Renderer::Common::TextureOptions;

namespace
{
    /// Records draws, with CPU-side resources (the software renderer's resource classes)
    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        std::vector<IMaterial *> drawnMaterials;
        int createMaterialCalls = 0;
        int createMeshCalls = 0;
        int updateMeshCalls = 0;
        int createTextureCalls = 0;

        std::vector<std::unique_ptr<Renderer::Software::SWMesh>> meshes;
        std::vector<std::unique_ptr<Renderer::Software::SWTexture>> textures;
        std::vector<std::unique_ptr<Renderer::Software::SWMaterial>> materials;
        Renderer::Software::SWShader textShader{Renderer::Software::SWShaderType::Text};

        IMesh *CreateMesh(const MeshData &meshData) override
        {
            ++createMeshCalls;
            auto mesh = std::make_unique<Renderer::Software::SWMesh>();
            mesh->vertices = meshData.vertices;
            mesh->indices = meshData.indices;
            meshes.push_back(std::move(mesh));
            return meshes.back().get();
        }

        bool UpdateMesh(IMesh *, const MeshData &) override
        {
            ++updateMeshCalls;
            return true;
        }

        void DestroyMesh(IMesh *mesh) override
        {
            std::erase_if(meshes, [mesh](const auto &p) { return p.get() == mesh; });
        }

        ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                const uint32_t channels) override
        {
            return CreateTexture(data, width, height, channels, TextureOptions::Default());
        }

        ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                const uint32_t channels, const TextureOptions &options) override
        {
            ++createTextureCalls;
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
            std::erase_if(textures, [texture](const auto &p) { return p.get() == texture; });
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
            std::erase_if(materials, [material](const auto &p) { return p.get() == material; });
        }

        [[nodiscard]] IShader *GetStandardTextShader() const override
        {
            return const_cast<Renderer::Software::SWShader *>(&textShader);
        }

        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *, const float *, IMaterial *material, const RenderState &) override
        {
            drawnMaterials.push_back(material);
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
        [[nodiscard]] IShader *GetStandardUnlitShader() const override { return nullptr; }
        [[nodiscard]] IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Recording"; }

        /// The material of the last draw, as the software material it is
        [[nodiscard]] const Renderer::Software::SWMaterial *LastMaterial() const
        {
            if (drawnMaterials.empty())
            {
                return nullptr;
            }
            return dynamic_cast<const Renderer::Software::SWMaterial *>(drawnMaterials.back());
        }
    };

    std::shared_ptr<Text::Font> DefaultFont()
    {
        auto font = Text::Font::GetDefault();
        EXPECT_NE(font, nullptr) << "the embedded default font didn't build";
        return font;
    }

    constexpr float kUnset = -12345.0f; // what a test reads for a uniform that was never set

    /// The effect uniforms a material holds (kUnset for any never set)
    struct MaterialEffects
    {
        float outline = kUnset;
        std::array<float, 4> outlineColor{};
        float softness = kUnset;
        std::array<float, 4> shadowColor{};
        std::array<float, 4> shadowOffset{};
        float shadowSoftness = kUnset;
    };

    MaterialEffects ReadEffects(const Renderer::Software::SWMaterial &material)
    {
        constexpr std::array<float, 4> unset{kUnset, kUnset, kUnset, kUnset};
        MaterialEffects m;
        m.outline = material.GetFloat("uOutline", kUnset);
        m.outlineColor = material.GetVec4("uOutlineColor", unset);
        m.softness = material.GetFloat("uSoftness", kUnset);
        m.shadowColor = material.GetVec4("uShadowColor", unset);
        m.shadowOffset = material.GetVec4("uShadowOffset", unset);
        m.shadowSoftness = material.GetFloat("uShadowSoftness", kUnset);
        return m;
    }

    void ExpectNoEffectUniforms(const Renderer::Software::SWMaterial &material)
    {
        const MaterialEffects m = ReadEffects(material);
        EXPECT_EQ(m.outline, 0.0f);
        EXPECT_EQ(m.softness, 0.0f);
        EXPECT_EQ(m.shadowSoftness, 0.0f);
        EXPECT_EQ(m.shadowColor[3], 0.0f) << "the shadow is off";
        EXPECT_EQ(m.shadowOffset[0], 0.0f);
        EXPECT_EQ(m.shadowOffset[1], 0.0f);
        EXPECT_NE(m.outlineColor[0], kUnset) << "uOutlineColor wasn't set";
    }

    void ExpectUniformsMatch(const Renderer::Software::SWMaterial &material, const EffectUniforms &expected)
    {
        const MaterialEffects m = ReadEffects(material);
        EXPECT_FLOAT_EQ(m.outline, expected.outline);
        EXPECT_FLOAT_EQ(m.outlineColor[0], expected.outlineColor.r);
        EXPECT_FLOAT_EQ(m.outlineColor[1], expected.outlineColor.g);
        EXPECT_FLOAT_EQ(m.outlineColor[2], expected.outlineColor.b);
        EXPECT_FLOAT_EQ(m.outlineColor[3], expected.outlineColor.a);
        EXPECT_FLOAT_EQ(m.softness, expected.softness);
        EXPECT_FLOAT_EQ(m.shadowColor[0], expected.shadowColor.r);
        EXPECT_FLOAT_EQ(m.shadowColor[3], expected.shadowColor.a);
        EXPECT_FLOAT_EQ(m.shadowOffset[0], expected.shadowOffsetU);
        EXPECT_FLOAT_EQ(m.shadowOffset[1], expected.shadowOffsetV);
        EXPECT_FLOAT_EQ(m.shadowSoftness, expected.shadowSoftness);
    }

    /// A set of effects with every setting on, inside the default font's limits
    Text::TextEffects SomeEffects()
    {
        Text::TextEffects effects;
        effects.outlineWidth = 0.04f;
        effects.outlineColor = Common::Color::Blue;
        effects.shadowOffset = Math::Vector2(0.03f, -0.02f);
        effects.shadowColor = Common::Color{0.0f, 0.0f, 0.0f, 0.5f};
        effects.shadowSoftness = 0.02f;
        effects.softness = 0.01f;
        return effects;
    }

    void ExpectSameEffects(const Text::TextEffects &actual, const Text::TextEffects &expected)
    {
        EXPECT_FLOAT_EQ(actual.outlineWidth, expected.outlineWidth);
        EXPECT_FLOAT_EQ(actual.outlineColor.r, expected.outlineColor.r);
        EXPECT_FLOAT_EQ(actual.outlineColor.g, expected.outlineColor.g);
        EXPECT_FLOAT_EQ(actual.outlineColor.b, expected.outlineColor.b);
        EXPECT_FLOAT_EQ(actual.outlineColor.a, expected.outlineColor.a);
        EXPECT_FLOAT_EQ(actual.shadowOffset.x, expected.shadowOffset.x);
        EXPECT_FLOAT_EQ(actual.shadowOffset.y, expected.shadowOffset.y);
        EXPECT_FLOAT_EQ(actual.shadowColor.r, expected.shadowColor.r);
        EXPECT_FLOAT_EQ(actual.shadowColor.a, expected.shadowColor.a);
        EXPECT_FLOAT_EQ(actual.shadowSoftness, expected.shadowSoftness);
        EXPECT_FLOAT_EQ(actual.softness, expected.softness);
    }

    void ExpectNoEffects(const Text::TextEffects &effects)
    {
        EXPECT_EQ(effects.outlineWidth, 0.0f);
        EXPECT_FALSE(effects.HasOutline());
        EXPECT_EQ(effects.shadowColor.a, 0.0f);
        EXPECT_FALSE(effects.HasShadow());
        EXPECT_EQ(effects.shadowOffset.x, 0.0f);
        EXPECT_EQ(effects.shadowOffset.y, 0.0f);
        EXPECT_EQ(effects.shadowSoftness, 0.0f);
        EXPECT_EQ(effects.softness, 0.0f);
    }

    /// Captures warnings that mention `needle` while it lives
    class WarningCapture
    {
    public:
        explicit WarningCapture(std::string needle) : _needle(std::move(needle))
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn && message.find(_needle) != std::string_view::npos)
                {
                    ++count;
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        int count = 0;

    private:
        std::string _needle;
        size_t _id = 0;
    };
}

// ============================================================================
// Settings
// ============================================================================

TEST(TextEffectsTest, EveryEffectIsOffByDefault)
{
    ExpectNoEffects(Text::TextEffects{});

    auto object = GameObject::Create("EffectsDefaults");
    const auto *text = object->AddComponent<Rendering::TextRenderer>();
    ASSERT_NE(text, nullptr);
    ExpectNoEffects(text->GetEffects());

    auto uiObject = GameObject::Create("EffectsDefaultsUI");
    const auto *uiText = uiObject->AddComponent<UI::UIText>();
    ASSERT_NE(uiText, nullptr);
    ExpectNoEffects(uiText->GetEffects());
}

TEST(TextEffectsTest, TheSettersSetTheEffects)
{
    auto object = GameObject::Create("EffectsSetters");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    text->SetOutline(0.05f, Common::Color::Red);
    text->SetShadow(Math::Vector2(0.02f, -0.03f), Common::Color::Black, 0.01f);
    text->SetSoftness(0.02f);
    const Text::TextEffects &effects = text->GetEffects();
    EXPECT_FLOAT_EQ(effects.outlineWidth, 0.05f);
    EXPECT_FLOAT_EQ(effects.outlineColor.r, 1.0f);
    EXPECT_FLOAT_EQ(effects.shadowOffset.x, 0.02f);
    EXPECT_FLOAT_EQ(effects.shadowOffset.y, -0.03f);
    EXPECT_FLOAT_EQ(effects.shadowColor.a, 1.0f);
    EXPECT_FLOAT_EQ(effects.shadowSoftness, 0.01f);
    EXPECT_FLOAT_EQ(effects.softness, 0.02f);
    EXPECT_TRUE(effects.HasOutline());
    EXPECT_TRUE(effects.HasShadow());

    auto uiObject = GameObject::Create("EffectsSettersUI");
    auto *uiText = uiObject->AddComponent<UI::UIText>();
    uiText->SetEffects(SomeEffects());
    ExpectSameEffects(uiText->GetEffects(), SomeEffects());
}

// ============================================================================
// Uniforms (TextDrawing::ResolveEffects)
// ============================================================================

TEST(TextEffectsTest, EffectsOffGiveTheNoEffectUniforms)
{
    const Text::AtlasSettings settings;
    const EffectUniforms none = ResolveEffects(Text::TextEffects{}, settings, 512, 256);
    EXPECT_EQ(none.outline, 0.0f);
    EXPECT_EQ(none.softness, 0.0f);
    EXPECT_EQ(none.shadowColor.a, 0.0f);
    EXPECT_EQ(none.shadowOffsetU, 0.0f);
    EXPECT_EQ(none.shadowOffsetV, 0.0f);
    EXPECT_EQ(none.shadowSoftness, 0.0f);
    EXPECT_FALSE(none.clamped);

    // A shadow with a transparent colour is off, whatever its offset and softness
    Text::TextEffects hiddenShadow;
    hiddenShadow.shadowOffset = Math::Vector2(0.05f, 0.05f);
    hiddenShadow.shadowSoftness = 0.05f;
    const EffectUniforms hidden = ResolveEffects(hiddenShadow, settings, 512, 256);
    EXPECT_EQ(hidden.shadowOffsetU, 0.0f);
    EXPECT_EQ(hidden.shadowOffsetV, 0.0f);
    EXPECT_EQ(hidden.shadowSoftness, 0.0f);
    EXPECT_EQ(hidden.shadowColor.a, 0.0f);

    // Negative and non-finite lengths count as 0, and aren't "clamped"
    Text::TextEffects odd;
    odd.outlineWidth = -1.0f;
    odd.softness = std::numeric_limits<float>::quiet_NaN();
    const EffectUniforms oddUniforms = ResolveEffects(odd, settings, 512, 256);
    EXPECT_EQ(oddUniforms.outline, 0.0f);
    EXPECT_EQ(oddUniforms.softness, 0.0f);
    EXPECT_FALSE(oddUniforms.clamped);
}

TEST(TextEffectsTest, LengthsInEmsBecomeAtlasDistancesAndUvOffsets)
{
    Text::AtlasSettings settings;
    settings.basePx = 48.0f;
    settings.spreadPx = 8;
    constexpr int atlasWidth = 512;
    constexpr int atlasHeight = 256;

    Text::TextEffects effects;
    effects.outlineWidth = 0.05f;
    effects.outlineColor = Common::Color::Blue;
    effects.softness = 0.02f;
    effects.shadowOffset = Math::Vector2(0.04f, -0.03f);
    effects.shadowColor = Common::Color::Red;
    effects.shadowSoftness = 0.04f;
    const EffectUniforms u = ResolveEffects(effects, settings, atlasWidth, atlasHeight);
    EXPECT_FALSE(u.clamped);

    // The atlas value changes by 0.5 over spreadPx pixels; an em is basePx pixels
    const float valuePerEm = 0.5f * 48.0f / 8.0f;
    EXPECT_FLOAT_EQ(u.outline, 0.05f * valuePerEm);
    EXPECT_FLOAT_EQ(u.outlineColor.b, 1.0f);
    EXPECT_FLOAT_EQ(u.softness, 0.01f * valuePerEm) << "the ramp's half width";
    EXPECT_FLOAT_EQ(u.shadowSoftness, 0.02f * valuePerEm);
    EXPECT_FLOAT_EQ(u.shadowColor.r, 1.0f);

    // +x right is +u; +y up is -v (the atlas is y-down), so a shadow moved down has a positive v offset
    EXPECT_FLOAT_EQ(u.shadowOffsetU, 0.04f * 48.0f / atlasWidth);
    EXPECT_FLOAT_EQ(u.shadowOffsetV, 0.03f * 48.0f / atlasHeight);
    EXPECT_GT(u.shadowOffsetU, 0.0f);
    EXPECT_GT(u.shadowOffsetV, 0.0f);
}

TEST(TextEffectsTest, EffectsAreClampedToTheSpread)
{
    Text::AtlasSettings settings;
    settings.basePx = 48.0f;
    settings.spreadPx = 8;
    const float maxEms = Rendering::TextDrawing::MaxEffectEms(settings);
    EXPECT_FLOAT_EQ(maxEms, Rendering::TextDrawing::kUsableSpread * 8.0f / 48.0f);
    const float valuePerEm = 0.5f * 48.0f / 8.0f;

    // An outline at the limit is kept as it is
    Text::TextEffects atLimit;
    atLimit.outlineWidth = maxEms;
    const EffectUniforms kept = ResolveEffects(atLimit, settings, 256, 256);
    EXPECT_FALSE(kept.clamped);
    EXPECT_FLOAT_EQ(kept.outline, maxEms * valuePerEm);

    // A wider one is cut to the limit, which keeps its outer edge above 0 (inside the glyph quads)
    Text::TextEffects wide;
    wide.outlineWidth = 1.0f;
    wide.softness = 0.1f;
    const EffectUniforms clamped = ResolveEffects(wide, settings, 256, 256);
    EXPECT_TRUE(clamped.clamped);
    EXPECT_FLOAT_EQ(clamped.outline, maxEms * valuePerEm);
    EXPECT_FLOAT_EQ(clamped.softness, 0.0f) << "the outline used the whole spread";
    EXPECT_GT(0.5f - clamped.outline - clamped.softness, 0.0f);

    // A shadow's offset gets what the outline and its softness leave, per axis, keeping its sign
    Text::TextEffects shadow;
    shadow.outlineWidth = 0.05f;
    shadow.shadowColor = Common::Color::Black;
    shadow.shadowSoftness = 0.02f;
    shadow.shadowOffset = Math::Vector2(-1.0f, 0.01f);
    const EffectUniforms s = ResolveEffects(shadow, settings, 256, 128);
    EXPECT_TRUE(s.clamped);
    const float offsetLimitEms = maxEms - 0.05f - 0.01f;
    EXPECT_NEAR(s.shadowOffsetU, -offsetLimitEms * 48.0f / 256.0f, 1e-6f);
    EXPECT_NEAR(s.shadowOffsetV, -0.01f * 48.0f / 128.0f, 1e-6f) << "y was within the limit";
}

TEST(TextEffectsTest, TheUniformsAreSetOnEveryDrawWithoutNewMaterials)
{
    const auto font = DefaultFont();
    ASSERT_NE(font, nullptr);
    RecordingRenderer renderer;
    auto object = GameObject::Create("EffectsUniforms");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    text->SetText("Hi");

    // Off: every effect uniform is set, to its no-effect value
    text->Render(&renderer);
    ASSERT_NE(renderer.LastMaterial(), nullptr);
    ExpectNoEffectUniforms(*renderer.LastMaterial());
    const int materials = renderer.createMaterialCalls;
    const int meshes = renderer.createMeshCalls;
    const int meshUpdates = renderer.updateMeshCalls;
    const int textures = renderer.createTextureCalls;

    // On: the resolved uniforms, on the same material
    text->SetEffects(SomeEffects());
    text->Render(&renderer);
    const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
    ExpectUniformsMatch(*renderer.LastMaterial(),
                        ResolveEffects(SomeEffects(), atlas.GetSettings(), atlas.GetWidth(), atlas.GetHeight()));

    // Back off
    text->SetEffects(Text::TextEffects{});
    text->Render(&renderer);
    ExpectNoEffectUniforms(*renderer.LastMaterial());

    // Effects never made a material, mesh or texture, or laid the text out again
    EXPECT_EQ(renderer.createMaterialCalls, materials);
    EXPECT_EQ(renderer.createMeshCalls, meshes);
    EXPECT_EQ(renderer.updateMeshCalls, meshUpdates);
    EXPECT_EQ(renderer.createTextureCalls, textures);

    text->CleanupRenderResources(&renderer);
}

TEST(TextEffectsTest, UITextSetsTheSameUniforms)
{
    const auto font = DefaultFont();
    ASSERT_NE(font, nullptr);
    RecordingRenderer renderer;
    auto object = GameObject::Create("EffectsUniformsUI");
    auto *text = object->AddComponent<UI::UIText>();
    text->SetText("Hi");
    const UI::Rect rect{0.0f, 0.0f, 200.0f, 50.0f};

    text->RenderUI(&renderer, rect, RenderState::Transparent());
    ASSERT_NE(renderer.LastMaterial(), nullptr);
    ExpectNoEffectUniforms(*renderer.LastMaterial());

    text->SetEffects(SomeEffects());
    text->RenderUI(&renderer, rect, RenderState::Transparent());
    const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
    ExpectUniformsMatch(*renderer.LastMaterial(),
                        ResolveEffects(SomeEffects(), atlas.GetSettings(), atlas.GetWidth(), atlas.GetHeight()));
    EXPECT_EQ(renderer.createMaterialCalls, 1);

    text->OnDestroy();
}

TEST(TextEffectsTest, ClampedEffectsWarnOncePerComponentType)
{
    const auto font = DefaultFont();
    ASSERT_NE(font, nullptr);
    // A component name no other test draws with, so this is the first warning for it in the process
    constexpr std::string_view name = "TextEffectsWarningTest";
    WarningCapture warnings{std::string(name)};

    RecordingRenderer renderer;
    Rendering::TextDrawing::DrawResources resources;
    resources.Bind(&renderer);
    const Text::TextLayout layout = font->Layout("Hi");
    const auto model = Math::Matrix<float, 4, 4>::identity();

    // Within the limits: no warning
    ASSERT_TRUE(resources.Draw(font, layout, 1, model.Data(), Common::Color::White, SomeEffects(),
                               RenderState::Transparent(), name));
    EXPECT_EQ(warnings.count, 0);

    // Past them: one warning, however many draws
    Text::TextEffects wide;
    wide.outlineWidth = 2.0f;
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(resources.Draw(font, layout, 1, model.Data(), Common::Color::White, wide,
                                   RenderState::Transparent(), name));
    }
    EXPECT_EQ(warnings.count, 1);

    // The uniforms are the clamped ones
    const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
    const EffectUniforms expected = ResolveEffects(wide, atlas.GetSettings(), atlas.GetWidth(), atlas.GetHeight());
    EXPECT_TRUE(expected.clamped);
    ExpectUniformsMatch(*renderer.LastMaterial(), expected);

    resources.Release(true);
}

// ============================================================================
// Saving and scripting
// ============================================================================

TEST(TextEffectsTest, EffectsSurviveASceneLoad)
{
    auto object = GameObject::Create("EffectsSaved");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    text->SetText("Saved");
    text->SetEffects(SomeEffects());
    auto uiObject = GameObject::Create("EffectsSavedUI");
    auto *uiText = uiObject->AddComponent<UI::UIText>();
    uiText->SetEffects(SomeEffects());

    const auto scene = Scene::Create("TextEffects_RoundTrip");
    scene->AddRootGameObject(object);
    scene->AddRootGameObject(uiObject);
    const nlohmann::json saved = scene->Serialize();
    const auto loadedScene = Scene::FromJSON(saved);
    ASSERT_NE(loadedScene, nullptr);

    const auto loadedObject = loadedScene->FindGameObject("EffectsSaved");
    ASSERT_NE(loadedObject, nullptr);
    const auto *loaded = loadedObject->GetComponent<Rendering::TextRenderer>();
    ASSERT_NE(loaded, nullptr);
    ExpectSameEffects(loaded->GetEffects(), SomeEffects());

    const auto loadedUIObject = loadedScene->FindGameObject("EffectsSavedUI");
    ASSERT_NE(loadedUIObject, nullptr);
    const auto *loadedUI = loadedUIObject->GetComponent<UI::UIText>();
    ASSERT_NE(loadedUI, nullptr);
    ExpectSameEffects(loadedUI->GetEffects(), SomeEffects());

    // Each setting has its own key
    const std::string dump = saved.dump();
    for (const char *key : {"\"_outlineWidth\"", "\"_outlineColor\"", "\"_shadowOffset\"", "\"_shadowColor\"",
                            "\"_shadowSoftness\"", "\"_softness\"", "\"outlineWidth\"", "\"shadowOffset\""})
    {
        EXPECT_NE(dump.find(key), std::string::npos) << key << " in " << dump;
    }

    // Data saved before effects existed (no effect keys) loads with every effect off
    auto oldObject = GameObject::Create("EffectsOld");
    auto *old = oldObject->AddComponent<Rendering::TextRenderer>();
    nlohmann::json withoutEffects = text->Serialize();
    for (const char *key : {"_outlineWidth", "_outlineColor", "_shadowOffset", "_shadowColor", "_shadowSoftness",
                            "_softness"})
    {
        withoutEffects.erase(std::string(key));
    }
    old->Deserialize(withoutEffects);
    ExpectNoEffects(old->GetEffects());
}

TEST(TextEffectsTest, LuaGetsAndSetsTheEffects)
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
        lua["text_fx_type"] = std::string(type);
        run(R"(
            text_fx_go = GameObject.Create("LuaTextEffects")
            text_fx = text_fx_go:AddComponent(text_fx_type)

            local width, color = text_fx:GetOutline()
            assert(width == 0, "outline off by default")
            local x, y, shadowColor, softness = text_fx:GetShadow()
            assert(x == 0 and y == 0 and shadowColor.a == 0 and softness == 0, "shadow off by default")
            assert(text_fx:GetSoftness() == 0)

            text_fx:SetOutline(0.05, Color.new(0, 0, 1, 1))
            text_fx:SetShadow(0.03, -0.02, Color.new(0, 0, 0, 0.5), 0.01)
            text_fx:SetSoftness(0.02)

            local outline, shadow
            text_fx_width, outline = text_fx:GetOutline()
            text_fx_outline_b = outline.b
            text_fx_x, text_fx_y, shadow, text_fx_shadow_softness = text_fx:GetShadow()
            text_fx_shadow_a = shadow.a
            text_fx_softness = text_fx:GetSoftness()

            -- The shadow's softness is optional (0)
            text_fx:SetShadow(0.01, 0.01, Color.new(1, 0, 0, 1))
            local _, _, _, defaultSoftness = text_fx:GetShadow()
            text_fx_default_softness = defaultSoftness
        )");
        EXPECT_FLOAT_EQ(lua["text_fx_width"].get<float>(), 0.05f);
        EXPECT_FLOAT_EQ(lua["text_fx_outline_b"].get<float>(), 1.0f);
        EXPECT_FLOAT_EQ(lua["text_fx_x"].get<float>(), 0.03f);
        EXPECT_FLOAT_EQ(lua["text_fx_y"].get<float>(), -0.02f);
        EXPECT_FLOAT_EQ(lua["text_fx_shadow_a"].get<float>(), 0.5f);
        EXPECT_FLOAT_EQ(lua["text_fx_shadow_softness"].get<float>(), 0.01f);
        EXPECT_FLOAT_EQ(lua["text_fx_softness"].get<float>(), 0.02f);
        EXPECT_FLOAT_EQ(lua["text_fx_default_softness"].get<float>(), 0.0f);
        run("text_fx_go = nil; text_fx = nil");
    }
}
