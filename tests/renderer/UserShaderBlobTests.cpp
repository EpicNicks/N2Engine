#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <renderer/common/BlockMaterial.hpp>
#include <renderer/common/GpuShaderInterface.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/UserShader.hpp>

#include "common/UserShaderFixture.hpp"

// The device-free half of the user shaders' stage 2 (orchestrator/proposals/user-shaders.md): the blob a backend is given,
// the limits checked before a driver sees it, the SPIR-V interface reader, and the BlockMaterial whose block is laid out by
// the cooked manifest instead of a built-in table. The fixtures are the cook's real output (scripts/cook-user-shader-fixtures.sh).

using namespace UserShaderTesting;
using Renderer::Common::ITexture;
using Renderer::Common::ReadFragmentVaryings;
using Renderer::Common::UserParam;
using Renderer::Common::UserParamType;
using Renderer::Common::UserShaderBlob;
using Renderer::Common::UserTextureDefault;
using Renderer::Common::ValidateUserShaderBlob;
using Renderer::Gpu::BlockMaterial;
using Renderer::Gpu::MaterialLayout;

namespace
{
    struct FakeTexture final : ITexture
    {
        [[nodiscard]] bool IsValid() const override { return true; }
        [[nodiscard]] std::uint32_t GetWidth() const override { return 1; }
        [[nodiscard]] std::uint32_t GetHeight() const override { return 1; }
        [[nodiscard]] std::uint32_t GetChannels() const override { return 4; }
    };

    float FloatAt(const BlockMaterial::Snapshot& snapshot, const std::uint32_t offset)
    {
        float value = 0;
        std::memcpy(&value, snapshot.block.data() + offset, sizeof(value));
        return value;
    }

    std::int32_t IntAt(const BlockMaterial::Snapshot& snapshot, const std::uint32_t offset)
    {
        std::int32_t value = 0;
        std::memcpy(&value, snapshot.block.data() + offset, sizeof(value));
        return value;
    }

    std::uint32_t VaryingsOf(const char* fixture)
    {
        std::uint32_t mask = 0xFFFFFFFFu;
        std::string error;
        EXPECT_TRUE(ReadFragmentVaryings(LoadFixture(fixture).fragmentSpirv, mask, error)) << fixture << ": " << error;
        return mask;
    }

    /// A valid blob to break one limit at a time: the "solid" fixture (six block members)
    UserShaderBlob ValidBlob()
    {
        return LoadFixture("solid");
    }

    struct Materials
    {
        std::vector<std::string> warnings;

        std::unique_ptr<BlockMaterial> Make(const UserShaderBlob& blob)
        {
            std::shared_ptr<const MaterialLayout> layout = MaterialLayout::FromUserShader(blob);
            EXPECT_NE(layout, nullptr);
            return std::make_unique<BlockMaterial>(
                layout, nullptr, nullptr, [this](const std::string& message) { warnings.push_back(message); },
                [](const ITexture*) -> std::uint64_t { return 7; });
        }
    };
}

TEST(UserShaderBlobTest, EveryCookedFixtureIsAcceptedAndWithinTheLimits)
{
    for (const char* name : {"solid", "textured", "ambient", "position", "varyings"})
    {
        const UserShaderBlob blob = LoadFixture(name);
        EXPECT_EQ(ValidateUserShaderBlob(blob), "") << name;
        EXPECT_LE(blob.blockSize, Renderer::Common::kUserShaderMaxBlockBytes) << name;
    }
    EXPECT_TRUE(LoadFixture("ambient").usesLighting);
    EXPECT_FALSE(LoadFixture("solid").usesLighting);
}

TEST(UserShaderBlobTest, TheVaryingsAFragmentStageReadsAreReadFromItsInterface)
{
    using namespace Renderer::Common;
    constexpr std::uint32_t all = kUserVaryingMasks - 1;
    // What a compiler lists in the interface is its own business (glslang lists every declared input, an optimiser would not),
    // so a cooked fixture reads as AT LEAST what it uses, and within the five
    const auto covers = [](const std::uint32_t mask, const std::uint32_t used) { return (mask & used) == used && (mask & ~all) == 0; };
    EXPECT_TRUE(covers(VaryingsOf("solid"), 0));
    EXPECT_TRUE(covers(VaryingsOf("ambient"), 0));
    EXPECT_TRUE(covers(VaryingsOf("textured"), kUserVaryingUv));
    EXPECT_TRUE(covers(VaryingsOf("position"), kUserVaryingUv | kUserVaryingColor));
    EXPECT_EQ(VaryingsOf("varyings"), all);

    // An interface cut down to some locations reads as exactly those
    const auto trimmed = [](const char* fixture, const std::uint32_t keep)
    {
        std::uint32_t mask = 0xFFFFFFFFu;
        std::string error;
        EXPECT_TRUE(ReadFragmentVaryings(WithFragmentInputsOnly(LoadFixture(fixture), keep).fragmentSpirv, mask, error)) << error;
        return mask;
    };
    EXPECT_EQ(trimmed("position", kUserVaryingUv | kUserVaryingColor), kUserVaryingUv | kUserVaryingColor);
    EXPECT_EQ(trimmed("position", kUserVaryingColor), kUserVaryingColor);
    EXPECT_EQ(trimmed("varyings", kUserVaryingNormal | kUserVaryingTangent), kUserVaryingNormal | kUserVaryingTangent);
    EXPECT_EQ(trimmed("solid", 0), 0u);
    EXPECT_EQ(ValidateUserShaderBlob(WithFragmentInputsOnly(LoadFixture("position"), kUserVaryingUv)), "") << "still a valid blob";
}

TEST(UserShaderBlobTest, TheVertexAttributesFollowTheVaryings)
{
    using namespace Renderer::Common;
    EXPECT_EQ(UserVertexAttributeMask(0), 0b00001u) << "position only";
    EXPECT_EQ(UserVertexAttributeMask(kUserVaryingUv), 0b00101u);
    EXPECT_EQ(UserVertexAttributeMask(kUserVaryingColor), 0b01001u);
    EXPECT_EQ(UserVertexAttributeMask(kUserVaryingWorldPos), 0b00001u) << "the world position needs only the position";
    EXPECT_EQ(UserVertexAttributeMask(kUserVaryingNormal), 0b00011u);
    EXPECT_EQ(UserVertexAttributeMask(kUserVaryingTangent), 0b10001u);
    EXPECT_EQ(UserVertexAttributeMask(kUserVaryingMasks - 1), 0b11111u);
}

TEST(UserShaderBlobTest, ASpirvModuleThatIsTruncatedOrOutsideTheContractIsRejectedBeforeADriverSeesIt)
{
    std::uint32_t mask = 0;
    std::string error;

    UserShaderBlob blob = ValidBlob();
    // Cut in the middle of the module's instructions
    std::vector<std::uint32_t> cut(blob.fragmentSpirv.begin(), blob.fragmentSpirv.begin() + 9);
    EXPECT_FALSE(ReadFragmentVaryings(cut, mask, error));
    EXPECT_FALSE(error.empty());
    // Shorter than the header
    EXPECT_FALSE(ReadFragmentVaryings(std::vector<std::uint32_t>{0x07230203u, 0, 0}, mask, error));
    // An instruction of zero words (would never advance)
    std::vector<std::uint32_t> zero = blob.fragmentSpirv;
    zero[5] = 0;
    EXPECT_FALSE(ReadFragmentVaryings(zero, mask, error));
    // An unlisted capability: the first OpCapability (opcode 17) becomes Int64 (11)
    std::vector<std::uint32_t> int64 = blob.fragmentSpirv;
    for (std::size_t at = 5; at < int64.size(); at += int64[at] >> 16)
    {
        if ((int64[at] & 0xFFFFu) == 17)
        {
            int64[at + 1] = 11;
            break;
        }
    }
    EXPECT_FALSE(ReadFragmentVaryings(int64, mask, error));
    EXPECT_NE(error.find("capability 11"), std::string::npos) << error;
    // No entry point at all
    std::vector<std::uint32_t> none(blob.fragmentSpirv.begin(), blob.fragmentSpirv.begin() + 5);
    EXPECT_FALSE(ReadFragmentVaryings(none, mask, error));
    EXPECT_NE(error.find("exactly one fragment entry point"), std::string::npos) << error;

    blob.fragmentSpirv = int64;
    EXPECT_NE(ValidateUserShaderBlob(blob).find("capability"), std::string::npos);
}

TEST(UserShaderBlobTest, EveryLimitOfTheBlobIsCheckedWithAMessageThatNamesIt)
{
    const auto problem = [](const auto& edit)
    {
        UserShaderBlob blob = ValidBlob();
        edit(blob);
        return ValidateUserShaderBlob(blob);
    };
    EXPECT_NE(problem([](UserShaderBlob& b) { b.contract = 2; }).find("contract 2"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.blockSize = 272; }).find("272 bytes"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { const UserParam first = b.params[0]; b.params.assign(65, first); }).find("65 parameters"),
              std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[1].name = b.params[0].name; }).find("declared twice"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[0].name = "n2_Tint"; }).find("reserved prefix"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[0].name = "gl_Thing"; }).find("reserved prefix"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[0].name = "bad name"; }).find("identifier"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[0].name = ""; }).find("identifier"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[0].name = std::string(65, 'a'); }).find("identifier"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[0].offset = 4; }).find("alignment"), std::string::npos) << "a vec4 at 4";
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[1].offset = 4; }).find("overlaps"), std::string::npos) << "a float inside the colour";
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[1].offset = 64; }).find("outside"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.params[1].size = 8; }).find("size"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.fragmentSpirv.clear(); }).find("empty"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.fragmentSpirv[0] = 0x12345678u; }).find("magic"), std::string::npos);
    EXPECT_NE(problem([](UserShaderBlob& b) { b.fragmentSpirv.resize(Renderer::Common::kUserShaderMaxSpirvBytes / 4 + 1, 0); })
                  .find("limit"), std::string::npos);

    // Texture slots: 0-4, one parameter each
    const auto textureProblem = [](const std::uint32_t first, const std::uint32_t second)
    {
        UserShaderBlob blob = ValidBlob();
        UserParam a;
        a.name = "t_A";
        a.type = UserParamType::Texture;
        a.slot = first;
        UserParam b = a;
        b.name = "t_B";
        b.slot = second;
        blob.params.push_back(a);
        blob.params.push_back(b);
        return ValidateUserShaderBlob(blob);
    };
    EXPECT_EQ(textureProblem(0, 4), "");
    EXPECT_NE(textureProblem(1, 1).find("two parameters"), std::string::npos);
    EXPECT_NE(textureProblem(0, 5).find("slot 5"), std::string::npos);
}

TEST(UserShaderBlobTest, AFreshUserMaterialHoldsTheManifestDefaultsAtTheManifestOffsets)
{
    Materials owner;
    const auto material = owner.Make(LoadFixture("solid"));
    const BlockMaterial::Snapshot snapshot = material->TakeSnapshot();
    EXPECT_TRUE(material->IsUserShader());
    EXPECT_EQ(snapshot.blockBytes, 64u);
    EXPECT_EQ(FloatAt(snapshot, 0), 1.0f);  // u_Color
    EXPECT_EQ(FloatAt(snapshot, 4), 0.0f);
    EXPECT_EQ(FloatAt(snapshot, 12), 1.0f);
    EXPECT_EQ(FloatAt(snapshot, 16), 1.0f); // u_Gain
    EXPECT_EQ(IntAt(snapshot, 44), 0);      // u_Flag, in the 16 bytes after the vec3
    EXPECT_EQ(IntAt(snapshot, 48), 2);      // u_Steps
    EXPECT_EQ(FloatAt(snapshot, 56), 0.25f); // u_Pair
    EXPECT_EQ(FloatAt(snapshot, 60), 0.5f);
    for (std::uint32_t at = 64; at < snapshot.block.size(); ++at)
    {
        EXPECT_EQ(snapshot.block[at], 0) << "past the block is zero: " << at;
    }
    EXPECT_TRUE(owner.warnings.empty());
}

TEST(UserShaderBlobTest, TheSettersWriteTheDeclaredMembersAndRefuseTheRest)
{
    Materials owner;
    const auto material = owner.Make(LoadFixture("solid"));
    material->SetColor("u_Color", 0.0f, 1.0f, 0.5f, 0.25f);
    material->SetFloat("u_Gain", 3.0f);
    material->SetVec3("u_Offset", 4.0f, 5.0f, 6.0f);
    material->SetInt("u_Flag", 1); // a bool parameter is an int in the block
    material->SetInt("u_Steps", 9);
    material->SetVec2("u_Pair", 7.0f, 8.0f);
    const BlockMaterial::Snapshot snapshot = material->TakeSnapshot();
    EXPECT_EQ(FloatAt(snapshot, 4), 1.0f);
    EXPECT_EQ(FloatAt(snapshot, 8), 0.5f);
    EXPECT_EQ(FloatAt(snapshot, 12), 0.25f);
    EXPECT_EQ(FloatAt(snapshot, 16), 3.0f);
    EXPECT_EQ(FloatAt(snapshot, 32), 4.0f);
    EXPECT_EQ(FloatAt(snapshot, 36), 5.0f);
    EXPECT_EQ(FloatAt(snapshot, 40), 6.0f);
    EXPECT_EQ(IntAt(snapshot, 44), 1);
    EXPECT_EQ(IntAt(snapshot, 48), 9);
    EXPECT_EQ(FloatAt(snapshot, 56), 7.0f);
    EXPECT_EQ(FloatAt(snapshot, 60), 8.0f);
    EXPECT_TRUE(owner.warnings.empty());

    material->SetFloat("u_Missing", 1.0f);
    material->SetFloat("u_Color", 1.0f); // a colour is a vec4: SetFloat does not apply
    material->SetFloat("uAlbedo", 1.0f); // a built-in shader's name is not in this block
    ASSERT_EQ(owner.warnings.size(), 3u);
    EXPECT_NE(owner.warnings[0].find("u_Missing"), std::string::npos);
    EXPECT_NE(owner.warnings[0].find("FixtureSolid"), std::string::npos);
    EXPECT_NE(owner.warnings[1].find("is a vec4"), std::string::npos);
    EXPECT_EQ(FloatAt(material->TakeSnapshot(), 0), 0.0f) << "the refused writes changed nothing (u_Color.r stays as set above)";
    EXPECT_EQ(FloatAt(material->TakeSnapshot(), 4), 1.0f);
}

TEST(UserShaderBlobTest, ATextureParameterIsASlotWithADeclaredDefault)
{
    Materials owner;
    const auto material = owner.Make(LoadFixture("textured"));
    EXPECT_EQ(material->TextureDefault(0), UserTextureDefault::White);
    EXPECT_EQ(material->TextureDefault(1), UserTextureDefault::Black);
    EXPECT_EQ(material->TextureDefault(2), UserTextureDefault::Normal);
    EXPECT_EQ(material->TextureDefault(3), UserTextureDefault::White) << "a slot no parameter uses reads white";
    EXPECT_EQ(material->TextureDefault(9), UserTextureDefault::White);
    EXPECT_EQ(material->TakeSnapshot().blockBytes, 16u) << "a lone colour";

    FakeTexture first;
    FakeTexture second;
    material->SetTextureSlot(0, &first);
    material->SetTextureSlot(1, &second);
    material->SetTextureSlot(5, &second); // past the slots: nothing
    EXPECT_EQ(material->GetTexture(), &first);
    EXPECT_EQ(material->GetAuxTexture(Renderer::Common::AuxTexture::Emissive), &second);
    EXPECT_EQ(material->GetAuxTexture(Renderer::Common::AuxTexture::Occlusion), nullptr);
}

TEST(UserShaderBlobTest, AShaderWithNoParametersHasAnEmptyBlockAndAValidDescriptorRange)
{
    Materials owner;
    const auto material = owner.Make(LoadFixture("position"));
    EXPECT_EQ(LoadFixture("position").blockSize, 0u);
    EXPECT_EQ(material->TakeSnapshot().blockBytes, 16u) << "a descriptor range of 0 is invalid, so the range is at least 16";
}

TEST(UserShaderBlobTest, ABuiltInMaterialKeepsItsOwn128ByteRange)
{
    using Renderer::Gpu::ShaderInterface::BuiltInShader;
    BlockMaterial unlit(BuiltInShader::Unlit, nullptr, nullptr, nullptr, nullptr);
    BlockMaterial lit(BuiltInShader::Lit, nullptr, nullptr, nullptr, nullptr);
    EXPECT_FALSE(unlit.IsUserShader());
    EXPECT_EQ(unlit.TakeSnapshot().blockBytes, Renderer::Gpu::ShaderInterface::kMaterialBlockRange);
    EXPECT_EQ(lit.TakeSnapshot().blockBytes, 128u);
}

TEST(UserShaderBlobTest, TheMaterialOutlivesTheShaderLayoutItWasMadeFrom)
{
    Materials owner;
    std::unique_ptr<BlockMaterial> material;
    {
        UserShaderBlob blob = LoadFixture("solid");
        material = owner.Make(blob);
        blob.params.clear(); // the names the layout views are its own copies
    }
    material->SetFloat("u_Gain", 2.0f);
    EXPECT_TRUE(owner.warnings.empty());
    EXPECT_EQ(FloatAt(material->TakeSnapshot(), 16), 2.0f);
}
