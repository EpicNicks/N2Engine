#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/common/UserShader.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "common/UserShaderFixture.hpp"
#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// User shaders on Vulkan (orchestrator/proposals/user-shaders.md, stage 2), at renderer level on a Vulkan device (Mesa lavapipe
// in CI) with validation on: the cooked fixtures of tests/renderer/fixtures/user-shaders/ (tools/n2shader's real output) become
// shaders through IRenderer::CreateUserShader and materials through CreateUserMaterial, and real frames are drawn and read
// back. What is pinned: the manifest-driven material block (defaults, setters, std140 offsets) reaches the shader; texture
// slots and their declared defaults; the opt-in lighting copy; every varying set of the engine's vertex stage (the 32 variants
// pair with the fragment stage's inputs without a validation warning); built-in and user draws in one frame (the descriptor
// range differs); the pipeline cache (ids, keys, LRU eviction and its bound); destroying a shader (retire, release at the
// next frame, id recycling); and the limits.

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::AuxTexture;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::IShader;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Common::UserShaderBlob;
using Renderer::Vulkan::VulkanRenderer;
using UserShaderTesting::LoadFixture;

namespace
{
    TextureOptions Nearest()
    {
        TextureOptions options;
        options.filter = TextureFilter::Nearest;
        options.wrap = TextureWrap::ClampToEdge;
        options.mipmaps = false;
        return options;
    }

    class CerrCapture
    {
    public:
        CerrCapture() : _old(std::cerr.rdbuf(_buffer.rdbuf())) {}
        ~CerrCapture() { std::cerr.rdbuf(_old); }
        CerrCapture(const CerrCapture&) = delete;
        CerrCapture& operator=(const CerrCapture&) = delete;
        [[nodiscard]] std::string Text() const { return _buffer.str(); }

    private:
        std::ostringstream _buffer;
        std::streambuf* _old;
    };

    float FloatAt(const std::vector<std::uint8_t>& block, const std::size_t offset)
    {
        float value = 0;
        std::memcpy(&value, block.data() + offset, sizeof(value));
        return value;
    }

    class VulkanUserShaderTest : public VulkanDrawTest
    {
    protected:
        IShader* MakeShader(const char* fixture)
        {
            IShader* shader = R().CreateUserShader(LoadFixture(fixture));
            EXPECT_NE(shader, nullptr) << fixture;
            return shader;
        }

        IMaterial* MakeMaterial(IShader* shader, Renderer::Common::ITexture* texture = nullptr)
        {
            IMaterial* material = R().CreateUserMaterial(shader, texture);
            EXPECT_NE(material, nullptr);
            return material;
        }

        /// The whole frame covered by one quad drawn with `material`
        Frame DrawFullFrame(IMaterial* material)
        {
            IMesh* mesh = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
            return RenderFrame([&] { R().DrawMesh(mesh, Identity, material, RenderState{}); });
        }

        Renderer::Common::ITexture* Pixel(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
        {
            const std::uint8_t data[4] = {r, g, b, 255};
            Renderer::Common::ITexture* texture = R().CreateTexture(data, 1, 1, 4, Nearest());
            EXPECT_NE(texture, nullptr);
            return texture;
        }

        std::vector<std::uint8_t> BlockOf(IMaterial* material)
        {
            std::vector<std::uint8_t> bytes;
            EXPECT_TRUE(R().GetMaterialBlockForTest(material, bytes));
            return bytes;
        }
    };
}

TEST_F(VulkanUserShaderTest, ADefaultMaterialDrawsTheManifestDefaults)
{
    IShader* shader = MakeShader("solid");
    IMaterial* material = MakeMaterial(shader);
    ASSERT_NE(material, nullptr);
    const Frame frame = DrawFullFrame(material);
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "u_Color (1,0,0,1) times u_Gain 1 plus u_Offset 0";
    EXPECT_EQ(material->GetShader(), shader);

    const std::vector<std::uint8_t> block = BlockOf(material);
    EXPECT_EQ(block.size(), 64u) << "the block's own range, not the built-ins' 128";
    EXPECT_EQ(FloatAt(block, 0), 1.0f);
    EXPECT_EQ(FloatAt(block, 16), 1.0f);
    EXPECT_EQ(FloatAt(block, 56), 0.25f);
}

TEST_F(VulkanUserShaderTest, ParametersSetByNameReachTheShaderAtTheirStd140Offsets)
{
    IMaterial* material = MakeMaterial(MakeShader("solid"));
    ASSERT_NE(material, nullptr);
    // u_Offset is a vec3 followed by u_Flag in the same 16 bytes: the packing the cook computed is the one the shader reads
    material->SetColor("u_Color", 0.0f, 1.0f, 0.0f, 1.0f);
    material->SetVec3("u_Offset", 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(DrawFullFrame(material).Count(Rgb{0, 255, 255}), Size * Size);

    material->SetInt("u_Flag", 1); // a bool parameter: inverted, (255, 0, 0)
    EXPECT_EQ(DrawFullFrame(material).Count(kRed), Size * Size);

    material->SetInt("u_Flag", 0);
    material->SetFloat("u_Gain", 0.0f); // the colour drops out: only the offset is left, blue
    EXPECT_EQ(DrawFullFrame(material).Count(kBlue), Size * Size);
}

TEST_F(VulkanUserShaderTest, AMaterialIsASnapshotAtTheDraw)
{
    IMaterial* material = MakeMaterial(MakeShader("solid"));
    ASSERT_NE(material, nullptr);
    IMesh* mesh = MakeMesh(TwoHalves());
    const Frame frame = RenderFrame(
        [&]
        {
            R().DrawMesh(mesh, Identity, material, RenderState{}, IndexRange{0, 6});
            material->SetColor("u_Color", 0.0f, 1.0f, 0.0f, 1.0f);
            R().DrawMesh(mesh, Identity, material, RenderState{}, IndexRange{6, 6});
        });
    EXPECT_TRUE(SameColour(frame.At(10, 32), kRed)) << "the left half was drawn before the change";
    EXPECT_TRUE(SameColour(frame.At(50, 32), kGreen));
}

TEST_F(VulkanUserShaderTest, TextureSlotsReadTheirDeclaredDefaultsUntilATextureIsSet)
{
    IMaterial* material = MakeMaterial(MakeShader("textured"));
    ASSERT_NE(material, nullptr);
    material->SetColor("u_Tint", 1.0f, 0.0f, 0.0f, 1.0f);
    // No texture is set: u_Tex reads white (so the tint shows), u_Add black (adds nothing), u_Normal the flat normal (its
    // red channel 0.5 is the alpha, 128 in the target)
    const Frame frame = DrawFullFrame(material);
    EXPECT_TRUE(SameColour(frame.At(32, 32), kRed));
    EXPECT_EQ(frame.AlphaAt(32, 32), 128);
    EXPECT_EQ(BlockOf(material).size(), 16u);
}

TEST_F(VulkanUserShaderTest, ATextureSetBySlotReplacesTheDefaultAndADestroyedOneFallsBackToIt)
{
    IMaterial* material = MakeMaterial(MakeShader("textured"));
    ASSERT_NE(material, nullptr);
    Renderer::Common::ITexture* green = Pixel(0, 255, 0);
    Renderer::Common::ITexture* blue = Pixel(0, 0, 255);

    material->SetTextureSlot(0, green); // u_Tex (the base texture), times the white tint
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), kGreen));

    material->SetTextureSlot(1, blue); // u_Add (the first AuxTexture slot), added
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), Rgb{0, 255, 255}));

    R().DestroyTexture(blue); // a destroyed texture reads the slot's declared default again (black)
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), kGreen));
    R().DestroyTexture(green);
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), kWhite));
}

TEST_F(VulkanUserShaderTest, AShaderThatAsksForLightingReadsTheSceneLightingOfItsDraw)
{
    IMaterial* material = MakeMaterial(MakeShader("ambient"));
    ASSERT_NE(material, nullptr);
    Renderer::Common::SceneLightingData first;
    first.ambientColor = N2Engine::Math::Vector3(Unorm(100), Unorm(50), Unorm(200));
    Renderer::Common::SceneLightingData second;
    second.ambientColor = N2Engine::Math::Vector3(Unorm(200), Unorm(100), Unorm(50));
    IMesh* mesh = MakeMesh(TwoHalves());
    const Frame frame = RenderFrame(
        [&]
        {
            // As OpenGL: a draw uses the lighting set by the last update before it
            R().UpdateSceneLighting(first, N2Engine::Math::Vector3(0.0f, 0.0f, 0.0f));
            R().DrawMesh(mesh, Identity, material, RenderState{}, IndexRange{0, 6});
            R().UpdateSceneLighting(second, N2Engine::Math::Vector3(0.0f, 0.0f, 0.0f));
            R().DrawMesh(mesh, Identity, material, RenderState{}, IndexRange{6, 6});
        });
    EXPECT_TRUE(SameColour(frame.At(10, 32), Rgb{100, 50, 200}));
    EXPECT_TRUE(SameColour(frame.At(50, 32), Rgb{200, 100, 50}));
}

TEST_F(VulkanUserShaderTest, TheVertexStageWritesTheVaryingsAShaderReads)
{
    // "position" reads the uv and the vertex colour (a 2-varying variant), no normal or world position
    {
        IMaterial* material = MakeMaterial(MakeShader("position"));
        ASSERT_NE(material, nullptr);
        EXPECT_EQ(BlockOf(material).size(), 16u) << "an empty block still has a valid range";
        const Frame frame = DrawFullFrame(material);
        EXPECT_LT(frame.At(3, 32).r, 20);
        EXPECT_GT(frame.At(60, 32).r, 230) << "red follows u, left to right";
        EXPECT_LT(frame.At(32, 3).g, 20) << "row 0 is the bottom, where v is 0";
        EXPECT_GT(frame.At(32, 60).g, 230);
        EXPECT_EQ(frame.At(32, 32).b, 255) << "the vertex colour's blue";
    }
    // "varyings" reads all five: the widest variant
    {
        IMaterial* material = MakeMaterial(MakeShader("varyings"));
        ASSERT_NE(material, nullptr);
        const Frame frame = DrawFullFrame(material);
        EXPECT_EQ(frame.At(32, 32).r, 255) << "the world normal's z";
        EXPECT_LT(frame.At(3, 32).g, 20);
        EXPECT_GT(frame.At(60, 32).g, 230) << "the world x remapped to 0..1";
        EXPECT_EQ(frame.At(32, 32).b, 0);
    }
}

TEST_F(VulkanUserShaderTest, BuiltInAndUserDrawsShareAFrameAndTheirDescriptorRangesDiffer)
{
    IMaterial* user = MakeMaterial(MakeShader("solid"));
    ASSERT_NE(user, nullptr);
    user->SetColor("u_Color", 0.0f, 1.0f, 0.0f, 1.0f);
    IMaterial* builtIn = Colour(1.0f, 0.0f, 0.0f);
    IMesh* mesh = MakeMesh(TwoHalves());
    const Frame frame = RenderFrame(
        [&]
        {
            R().DrawMesh(mesh, Identity, builtIn, RenderState{}, IndexRange{0, 6});
            R().DrawMesh(mesh, Identity, user, RenderState{}, IndexRange{6, 6});
        });
    EXPECT_TRUE(SameColour(frame.At(10, 32), kRed));
    EXPECT_TRUE(SameColour(frame.At(50, 32), kGreen));
    // The built-in material's block is unchanged: 128 bytes, the descriptor range every built-in shader has always had
    EXPECT_EQ(BlockOf(builtIn).size(), Renderer::Gpu::ShaderInterface::kMaterialBlockRange);
    EXPECT_EQ(BlockOf(user).size(), 64u);
}

TEST_F(VulkanUserShaderTest, ABlendedUserDrawBlendsAndADepthWriteTestsLikeABuiltIn)
{
    IMaterial* material = MakeMaterial(MakeShader("solid"));
    ASSERT_NE(material, nullptr);
    material->SetColor("u_Color", 0.0f, 0.0f, 1.0f, 0.5f);
    IMaterial* background = Colour(1.0f, 0.0f, 0.0f);
    IMesh* back = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f));
    IMesh* front = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    RenderState blend;
    blend.blend = true;
    blend.depthWrite = false;
    const Frame frame = RenderFrame(
        [&]
        {
            R().DrawMesh(back, Identity, background, RenderState{});
            R().DrawMesh(front, Identity, material, blend);
        });
    const Rgb mixed = frame.At(32, 32);
    EXPECT_NEAR(mixed.r, 128, 2) << "half red under, half blue over";
    EXPECT_NEAR(mixed.b, 128, 2);
}

TEST_F(VulkanUserShaderTest, EachShaderHasItsOwnPipelineKeyAndTheBuiltInsKeepTheirIds)
{
    IShader* a = MakeShader("solid");
    IShader* b = MakeShader("textured");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const RenderState state;
    const std::uint32_t unlit = R().GetPipelineKeyForTest(R().GetStandardUnlitShader(), state, false);
    const std::uint32_t lit = R().GetPipelineKeyForTest(R().GetStandardLitShader(), state, false);
    const std::uint32_t text = R().GetPipelineKeyForTest(R().GetStandardTextShader(), state, false);
    const std::uint32_t keyA = R().GetPipelineKeyForTest(a, state, false);
    const std::uint32_t keyB = R().GetPipelineKeyForTest(b, state, false);
    EXPECT_EQ(unlit & 0xFFFFu, 0u);
    EXPECT_EQ(lit & 0xFFFFu, 1u);
    EXPECT_EQ(text & 0xFFFFu, 2u);
    EXPECT_GE(keyA & 0xFFFFu, 3u) << "user shaders take the ids after the three built-ins";
    EXPECT_GE(keyB & 0xFFFFu, 3u);
    EXPECT_NE(keyA & 0xFFFFu, keyB & 0xFFFFu);
    EXPECT_EQ(unlit >> 16, keyA >> 16) << "the same state packs the same bits above the shader id";
    EXPECT_EQ(R().GetPipelineKeyForTest(nullptr, state, false), 0xFFFFFFFFu);

    RenderState other = state;
    other.blend = true;
    EXPECT_NE(R().GetPipelineKeyForTest(a, other, false), keyA);
    EXPECT_NE(R().GetPipelineKeyForTest(a, state, true), keyA);
}

TEST_F(VulkanUserShaderTest, PipelinesAreMadeOnceAndCachedPerShaderAndState)
{
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    RenderState state;
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 0u);
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, state, false));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 1u);
    const std::uint32_t attempts = R().GetPipelineCreateAttemptCountForTest();
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, state, false));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 1u);
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), attempts) << "a cached pipeline is not made again";
    state.cull = CullMode::None;
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, state, false));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 2u);
    EXPECT_FALSE(R().CreateUserPipelineForTest(R().GetStandardUnlitShader(), state, false)) << "not a user shader";

    // A built-in shader's pipeline is not counted among the user ones
    EXPECT_TRUE(R().CreatePipelineForTest(Renderer::Gpu::ShaderInterface::BuiltInShader::Unlit, state, false));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 2u);
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u);
}

TEST_F(VulkanUserShaderTest, AFailedPipelineIsRememberedAndNotRetried)
{
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    R().SetFailPipelineCreationForTest(true);
    CerrCapture capture;
    EXPECT_FALSE(R().CreateUserPipelineForTest(a, RenderState{}, false));
    const std::uint32_t attempts = R().GetPipelineCreateAttemptCountForTest();
    EXPECT_FALSE(R().CreateUserPipelineForTest(a, RenderState{}, false));
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), attempts);
    R().SetFailPipelineCreationForTest(false);
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 0u);
}

TEST_F(VulkanUserShaderTest, ADestroyedShaderIsRetiredAtOnceAndReleasedAtTheNextFrame)
{
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    IMaterial* material = MakeMaterial(a);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(DrawFullFrame(material).Count(kRed), Size * Size);
    EXPECT_EQ(R().GetUserShaderCountForTest(), 1u);
    EXPECT_GE(R().GetUserPipelineCountForTest(), 1u);
    EXPECT_TRUE(R().IsValidShader(a));
    const std::uint32_t oldId = R().GetPipelineKeyForTest(a, RenderState{}, false) & 0xFFFFu;

    EXPECT_FALSE(R().DestroyShaderProgram(R().GetStandardUnlitShader())) << "the built-ins are the renderer's";
    EXPECT_FALSE(R().DestroyShaderProgram(nullptr));
    EXPECT_TRUE(R().DestroyShaderProgram(a));
    EXPECT_FALSE(R().DestroyShaderProgram(a)) << "already retired";
    EXPECT_EQ(R().GetUserShaderCountForTest(), 0u);
    EXPECT_FALSE(R().IsValidShader(a));
    EXPECT_EQ(R().CreateUserMaterial(a, nullptr), nullptr);

    // The material of a destroyed shader draws nothing; the frame start releases the shader's pipelines
    const Frame frame = DrawFullFrame(material);
    EXPECT_EQ(frame.Count(kBlack), Size * Size);
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 0u);

    // The id is recycled, and the new shader draws (nothing of the old one's pipelines or failures is left)
    IShader* b = MakeShader("solid");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(R().GetPipelineKeyForTest(b, RenderState{}, false) & 0xFFFFu, oldId);
    IMaterial* next = MakeMaterial(b);
    ASSERT_NE(next, nullptr);
    next->SetColor("u_Color", 0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_EQ(DrawFullFrame(next).Count(kBlue), Size * Size);
    R().DestroyMaterial(material); // a material may be destroyed after its shader
}

TEST_F(VulkanUserShaderTest, UserPipelinesPastTheLimitAreEvictedLeastRecentlyUsedAtTheNextFrame)
{
    R().SetMaxUserPipelinesForTest(4);
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    std::vector<RenderState> states;
    for (const CullMode cull : {CullMode::None, CullMode::Back, CullMode::Front})
    {
        for (const bool blend : {false, true})
        {
            for (const bool depthTest : {true, false})
            {
                RenderState state;
                state.cull = cull;
                state.blend = blend;
                state.depthTest = depthTest;
                states.push_back(state);
            }
        }
    }
    ASSERT_GE(states.size(), 12u);
    CerrCapture capture;
    // Inside one frame the cache may grow to twice the limit, no further
    for (std::size_t i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[i], false)) << i;
    }
    EXPECT_FALSE(R().CreateUserPipelineForTest(a, states[8], false)) << "past twice the limit in one frame";
    EXPECT_NE(capture.Text().find("pipeline limit"), std::string::npos) << capture.Text();
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 8u);

    // The next frame start evicts down to three quarters of the limit
    R().BeginFrame();
    R().EndFrame();
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 3u);

    // What was evicted is made again when a draw needs it
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[0], false));
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[8], false));
}

TEST_F(VulkanUserShaderTest, ThereAreAtMostAFixedNumberOfLiveUserShaders)
{
    CerrCapture capture;
    std::vector<IShader*> shaders;
    const UserShaderBlob blob = LoadFixture("solid");
    for (std::size_t i = 0; i < Renderer::Common::kMaxUserShaders; ++i)
    {
        IShader* shader = R().CreateUserShader(blob);
        ASSERT_NE(shader, nullptr) << i;
        shaders.push_back(shader);
    }
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);
    EXPECT_NE(capture.Text().find("256 user shaders"), std::string::npos) << capture.Text();

    // A slot is free again once the frame start has released the destroyed shader
    EXPECT_TRUE(R().DestroyShaderProgram(shaders[10]));
    EXPECT_EQ(R().CreateUserShader(blob), nullptr) << "the retired shader still holds its slot until the frame start";
    R().BeginFrame();
    R().EndFrame();
    EXPECT_NE(R().CreateUserShader(blob), nullptr);
}

TEST_F(VulkanUserShaderTest, ABlobThatBreaksALimitIsRefusedBeforeTheDriverSeesIt)
{
    CerrCapture capture;
    UserShaderBlob blob = LoadFixture("solid");
    blob.fragmentSpirv[0] = 0;
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);
    EXPECT_NE(capture.Text().find("rejected"), std::string::npos) << capture.Text();
    EXPECT_NE(capture.Text().find("magic number"), std::string::npos) << capture.Text();

    blob = LoadFixture("solid");
    blob.blockSize = 4096;
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);
    EXPECT_EQ(R().GetUserShaderCountForTest(), 0u);

    // A material needs a live user shader of this renderer
    EXPECT_EQ(R().CreateUserMaterial(nullptr, nullptr), nullptr);
    EXPECT_EQ(R().CreateUserMaterial(R().GetStandardUnlitShader(), nullptr), nullptr);
}
