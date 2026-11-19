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
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "common/UserShaderFixture.hpp"
#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"

// User shaders on WebGPU (orchestrator/proposals/user-shaders.md, stage 3), at renderer level on a WebGPU device (Dawn over Mesa
// lavapipe in CI), mirroring tests/renderer/vulkan/VulkanUserShaderTests.cpp with the SAME fixtures and the SAME expected colours,
// which is the golden comparison with Vulkan: each cooked fixture (tools/n2shader's real output, the WGSL Tint wrote from the
// SPIR-V) draws exactly what the Vulkan test of it asserts, so the two backends agree on every pixel those tests check (exact
// colours, never a tolerance). What is pinned: the manifest-driven material block (defaults, setters, std140 offsets as the WGSL
// reads them); texture slots and their declared defaults; the opt-in lighting copy; the varyings of the engine's vertex stage; a
// model matrix reaching the shader exactly once transposed (the transpose-twice trap of the proposal's stage 3); built-in and
// user draws in one frame (the material ranges differ); the pipeline cache (ids, keys, LRU eviction and its bound); destroying a
// shader; the limits; and WGSL the device rejects (captured, never an uncaptured error: the fixture fails a test on any).

using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::IShader;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Common::UserShaderBlob;
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

    class WebGpuUserShaderTest : public WebGpuDrawTest
    {
    protected:
        IShader* MakeShader(const char* fixture)
        {
            IShader* shader = R().CreateUserShader(LoadFixture(fixture));
            EXPECT_NE(shader, nullptr) << fixture;
            return shader;
        }

        IMaterial* MakeMaterial(IShader* shader, ITexture* texture = nullptr)
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

        ITexture* Pixel(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
        {
            const std::uint8_t data[4] = {r, g, b, 255};
            ITexture* texture = R().CreateTexture(data, 1, 1, 4, Nearest());
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

TEST_F(WebGpuUserShaderTest, ADefaultMaterialDrawsTheManifestDefaults)
{
    IShader* shader = MakeShader("solid");
    IMaterial* material = MakeMaterial(shader);
    ASSERT_NE(material, nullptr);
    const Frame frame = DrawFullFrame(material);
    EXPECT_EQ(frame.Count(kRed), Size * Size) << "u_Color (1,0,0,1) times u_Gain 1 plus u_Offset 0";
    EXPECT_EQ(material->GetShader(), shader);
    EXPECT_TRUE(R().IsValidShader(shader));

    const std::vector<std::uint8_t> block = BlockOf(material);
    EXPECT_EQ(block.size(), 64u) << "the block's own range, not the built-ins' 128";
    EXPECT_EQ(FloatAt(block, 0), 1.0f);
    EXPECT_EQ(FloatAt(block, 16), 1.0f);
    EXPECT_EQ(FloatAt(block, 56), 0.25f);
}

TEST_F(WebGpuUserShaderTest, ParametersSetByNameReachTheShaderAtTheirStd140Offsets)
{
    IMaterial* material = MakeMaterial(MakeShader("solid"));
    ASSERT_NE(material, nullptr);
    // u_Offset is a vec3 followed by u_Flag in the same 16 bytes: the packing the cook computed is the one the WGSL reads
    material->SetColor("u_Color", 0.0f, 1.0f, 0.0f, 1.0f);
    material->SetVec3("u_Offset", 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(DrawFullFrame(material).Count(Rgb{0, 255, 255}), Size * Size);

    material->SetInt("u_Flag", 1); // a bool parameter: inverted, (255, 0, 0)
    EXPECT_EQ(DrawFullFrame(material).Count(kRed), Size * Size);

    material->SetInt("u_Flag", 0);
    material->SetFloat("u_Gain", 0.0f); // the colour drops out: only the offset is left, blue
    EXPECT_EQ(DrawFullFrame(material).Count(kBlue), Size * Size);
}

TEST_F(WebGpuUserShaderTest, AMaterialIsASnapshotAtTheDraw)
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

TEST_F(WebGpuUserShaderTest, TextureSlotsReadTheirDeclaredDefaultsUntilATextureIsSet)
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

TEST_F(WebGpuUserShaderTest, ATextureSetBySlotReplacesTheDefaultAndADestroyedOneFallsBackToIt)
{
    IMaterial* material = MakeMaterial(MakeShader("textured"));
    ASSERT_NE(material, nullptr);
    ITexture* green = Pixel(0, 255, 0);
    ITexture* blue = Pixel(0, 0, 255);

    material->SetTextureSlot(0, green); // u_Tex (the base texture), times the white tint
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), kGreen));

    material->SetTextureSlot(1, blue); // u_Add (the first AuxTexture slot), added
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), Rgb{0, 255, 255}));

    R().DestroyTexture(blue); // a destroyed texture reads the slot's declared default again (black)
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), kGreen));
    R().DestroyTexture(green);
    EXPECT_TRUE(SameColour(DrawFullFrame(material).At(32, 32), kWhite));
}

TEST_F(WebGpuUserShaderTest, AShaderThatAsksForLightingReadsTheSceneLightingOfItsDraw)
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

TEST_F(WebGpuUserShaderTest, TheVertexStageWritesTheVaryingsAShaderReads)
{
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

TEST_F(WebGpuUserShaderTest, AModelMatrixReachesTheUserVertexStageTransposedExactlyOnce)
{
    // The transpose-twice trap (proposal stage 3): the backend transposes each matrix on upload, so the user vertex stage and the
    // cooked fragment module must both read the block column-major. A second transpose would lose a translation. "varyings" writes
    // the world x as green: translated by 0.5, a pixel at NDC x shows green = x * 0.5 + 0.5 (the world x of a vertex is its
    // translated x, which is the pixel's NDC x), and the quad no longer covers the left quarter of the frame
    IMaterial* material = MakeMaterial(MakeShader("varyings"));
    ASSERT_NE(material, nullptr);
    IMesh* mesh = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    float model[16];
    Translation(model, 0.5f, 0.0f, 0.0f);
    const Frame frame = RenderFrame([&] { R().DrawMesh(mesh, model, material, RenderState{}); });
    EXPECT_TRUE(SameColour(frame.At(4, 32), kBlack)) << "the quad moved right: nothing covers the left edge";
    EXPECT_NEAR(frame.At(50, 32).g, 199, 4) << "NDC x 0.5625 -> 0.78";
    EXPECT_NEAR(frame.At(32, 32).g, 128, 4);
}

TEST_F(WebGpuUserShaderTest, BuiltInAndUserDrawsShareAFrameAndTheirMaterialRangesDiffer)
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
            R().DrawMesh(mesh, Identity, builtIn, RenderState{}, IndexRange{0, 6});
        });
    EXPECT_TRUE(SameColour(frame.At(10, 32), kRed));
    EXPECT_TRUE(SameColour(frame.At(50, 32), kGreen));
    EXPECT_EQ(BlockOf(builtIn).size(), Renderer::Gpu::ShaderInterface::kMaterialBlockRange)
        << "the built-in material's block is unchanged: 128 bytes";
    EXPECT_EQ(BlockOf(user).size(), 64u);
}

TEST_F(WebGpuUserShaderTest, ABlendedUserDrawBlendsLikeABuiltIn)
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

TEST_F(WebGpuUserShaderTest, EachShaderHasItsOwnPipelineKeyAndTheBuiltInsKeepTheirIds)
{
    IShader* a = MakeShader("solid");
    IShader* b = MakeShader("textured");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const RenderState state;
    const std::uint32_t unlit = R().GetPipelineKeyForTest(R().GetStandardUnlitShader(), state);
    const std::uint32_t lit = R().GetPipelineKeyForTest(R().GetStandardLitShader(), state);
    const std::uint32_t text = R().GetPipelineKeyForTest(R().GetStandardTextShader(), state);
    const std::uint32_t keyA = R().GetPipelineKeyForTest(a, state);
    const std::uint32_t keyB = R().GetPipelineKeyForTest(b, state);
    EXPECT_EQ(unlit & 0xFFFFu, 0u);
    EXPECT_EQ(lit & 0xFFFFu, 1u);
    EXPECT_EQ(text & 0xFFFFu, 2u);
    EXPECT_GE(keyA & 0xFFFFu, 3u) << "user shaders take the ids after the three built-ins";
    EXPECT_GE(keyB & 0xFFFFu, 3u);
    EXPECT_NE(keyA & 0xFFFFu, keyB & 0xFFFFu);
    EXPECT_EQ(R().GetPipelineKeyForTest(nullptr, state), 0xFFFFFFFFu);

    RenderState other = state;
    other.blend = true;
    EXPECT_NE(R().GetPipelineKeyForTest(a, other), keyA);
}

TEST_F(WebGpuUserShaderTest, PipelinesAreMadeOnceAndCachedPerShaderAndState)
{
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    RenderState state;
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 0u);
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, state));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 1u);
    const std::uint32_t attempts = R().GetPipelineCreateAttemptCountForTest();
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, state));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 1u);
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), attempts) << "a cached pipeline is not made again";
    state.cull = CullMode::None;
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, state));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 2u);
    EXPECT_FALSE(R().CreateUserPipelineForTest(R().GetStandardUnlitShader(), state)) << "not a user shader";

    // A built-in shader's pipeline is not counted among the user ones
    EXPECT_TRUE(R().CreatePipelineForTest(Renderer::Gpu::ShaderInterface::BuiltInShader::Unlit, state));
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 2u);
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u);
}

TEST_F(WebGpuUserShaderTest, AFailedPipelineIsRememberedAndNotRetried)
{
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    R().SetFailPipelineCreationForTest(true);
    CerrCapture capture;
    EXPECT_FALSE(R().CreateUserPipelineForTest(a, RenderState{}));
    const std::uint32_t attempts = R().GetPipelineCreateAttemptCountForTest();
    EXPECT_FALSE(R().CreateUserPipelineForTest(a, RenderState{}));
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), attempts);
    R().SetFailPipelineCreationForTest(false);
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 0u);
}

TEST_F(WebGpuUserShaderTest, ADestroyedShaderIsReleasedAtOnceAndItsIdIsRecycled)
{
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    IMaterial* material = MakeMaterial(a);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(DrawFullFrame(material).Count(kRed), Size * Size);
    EXPECT_EQ(R().GetUserShaderCountForTest(), 1u);
    EXPECT_GE(R().GetUserPipelineCountForTest(), 1u);
    const std::uint32_t oldId = R().GetPipelineKeyForTest(a, RenderState{}) & 0xFFFFu;

    EXPECT_FALSE(R().DestroyShaderProgram(R().GetStandardUnlitShader())) << "the built-ins are the renderer's";
    EXPECT_FALSE(R().DestroyShaderProgram(nullptr));
    EXPECT_TRUE(R().DestroyShaderProgram(a));
    EXPECT_FALSE(R().DestroyShaderProgram(a)) << "already destroyed";
    EXPECT_EQ(R().GetUserShaderCountForTest(), 0u);
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 0u) << "its pipelines went with it";
    EXPECT_FALSE(R().IsValidShader(a));
    EXPECT_EQ(R().CreateUserMaterial(a, nullptr), nullptr);

    // The material of a destroyed shader draws nothing
    EXPECT_EQ(DrawFullFrame(material).Count(kBlack), Size * Size);

    // The id is recycled, and the new shader draws
    IShader* b = MakeShader("solid");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(R().GetPipelineKeyForTest(b, RenderState{}) & 0xFFFFu, oldId);
    IMaterial* next = MakeMaterial(b);
    ASSERT_NE(next, nullptr);
    next->SetColor("u_Color", 0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_EQ(DrawFullFrame(next).Count(kBlue), Size * Size);
    R().DestroyMaterial(material); // a material may be destroyed after its shader
}

TEST_F(WebGpuUserShaderTest, UserPipelinesPastTheLimitAreEvictedLeastRecentlyUsedFirst)
{
    R().SetMaxUserPipelinesForTest(4);
    IShader* a = MakeShader("solid");
    ASSERT_NE(a, nullptr);
    std::vector<RenderState> states;
    for (const CullMode cull : {CullMode::None, CullMode::Back, CullMode::Front})
    {
        for (const bool blend : {false, true})
        {
            RenderState state;
            state.cull = cull;
            state.blend = blend;
            states.push_back(state);
        }
    }
    CerrCapture capture;
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[i])) << i;
    }
    EXPECT_EQ(R().GetUserPipelineCountForTest(), 4u);
    // Touch the first so it is the most recently used, then make a fifth: the cache drops to three quarters of the limit
    // (3), least recently used first (states 1 and 2 go, then it adds the new one)
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[0]));
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[4]));
    EXPECT_LE(R().GetUserPipelineCountForTest(), 4u);
    EXPECT_NE(capture.Text().find("pipeline limit"), std::string::npos) << capture.Text();
    const std::uint32_t attempts = R().GetPipelineCreateAttemptCountForTest();
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[0]));
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), attempts) << "the most recently used survived";
    EXPECT_TRUE(R().CreateUserPipelineForTest(a, states[1]));
    EXPECT_GT(R().GetPipelineCreateAttemptCountForTest(), attempts) << "an evicted pipeline is made again when a draw needs it";
}

TEST_F(WebGpuUserShaderTest, ThereAreAtMostAFixedNumberOfLiveUserShaders)
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

    EXPECT_TRUE(R().DestroyShaderProgram(shaders[10]));
    EXPECT_NE(R().CreateUserShader(blob), nullptr) << "a destroyed shader frees its slot at once";
}

TEST_F(WebGpuUserShaderTest, ABlobThatBreaksALimitIsRefusedBeforeTheDeviceSeesIt)
{
    CerrCapture capture;
    UserShaderBlob blob = LoadFixture("solid");
    blob.fragmentWgsl.clear();
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);
    EXPECT_NE(capture.Text().find("rejected"), std::string::npos) << capture.Text();
    EXPECT_NE(capture.Text().find("WebGPU fragment payload is empty"), std::string::npos) << capture.Text();

    blob = LoadFixture("solid");
    blob.fragmentWgsl = "not wgsl at all";
    EXPECT_EQ(R().CreateUserShader(blob), nullptr) << "no @fragment entry point";

    blob = LoadFixture("solid");
    blob.blockSize = 4096;
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);
    EXPECT_EQ(R().GetUserShaderCountForTest(), 0u);

    // A blob cooked for Vulkan only has no WGSL
    blob = LoadFixture("solid");
    blob.fragmentWgsl.clear();
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);

    EXPECT_EQ(R().CreateUserMaterial(nullptr, nullptr), nullptr);
    EXPECT_EQ(R().CreateUserMaterial(R().GetStandardUnlitShader(), nullptr), nullptr);
}

TEST_F(WebGpuUserShaderTest, WgslTheDeviceRejectsIsCapturedNotCountedAsAnUncapturedError)
{
    // Looks like a fragment shader to the cheap checks, but is not valid WGSL: the device says so, the scope captures it, the
    // fixture's zero-uncaptured-errors check stays green, and no shader is made
    CerrCapture capture;
    UserShaderBlob blob = LoadFixture("solid");
    blob.fragmentWgsl = "@fragment fn main() -> @location(0) vec4<f32> { return vec3<f32>(1.0); }\n";
    EXPECT_EQ(R().CreateUserShader(blob), nullptr);
    EXPECT_NE(capture.Text().find("the device rejected the WGSL"), std::string::npos) << capture.Text();
    EXPECT_EQ(R().GetUserShaderCountForTest(), 0u);
}

TEST_F(WebGpuUserShaderTest, WgslThatDoesNotFitThePipelineLayoutFailsOnlyItsPipeline)
{
    // Valid WGSL, but it reads a binding the renderer's pipeline layout does not have (group 2): the module is accepted, the
    // pipeline is not, the error is captured, the key is a remembered failure and the frame draws nothing for it
    CerrCapture capture;
    UserShaderBlob blob = LoadFixture("solid");
    blob.fragmentWgsl =
        "@group(2) @binding(0) var<uniform> extra : vec4<f32>;\n"
        "@fragment fn main(@location(0) uv : vec2<f32>, @location(1) c : vec4<f32>, @location(2) w : vec3<f32>,\n"
        "                   @location(3) n : vec3<f32>, @location(4) t : vec4<f32>) -> @location(0) vec4<f32> { return extra; }\n";
    IShader* shader = R().CreateUserShader(blob);
    ASSERT_NE(shader, nullptr);
    EXPECT_FALSE(R().CreateUserPipelineForTest(shader, RenderState{}));
    const std::uint32_t attempts = R().GetPipelineCreateAttemptCountForTest();
    EXPECT_FALSE(R().CreateUserPipelineForTest(shader, RenderState{}));
    EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), attempts) << "not retried";
    IMaterial* material = MakeMaterial(shader);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(DrawFullFrame(material).Count(kBlack), Size * Size);
    EXPECT_NE(capture.Text().find("could not create the render pipeline"), std::string::npos) << capture.Text();
}
