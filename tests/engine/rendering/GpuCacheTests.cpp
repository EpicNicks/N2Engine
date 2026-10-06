#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <renderer/common/RenderState.hpp>
#include <renderer/common/TextureOptions.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/text/Font.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"

#include "TextureTestSupport.hpp"

// GpuCache: one GPU texture per (renderer, asset), shared and refcounted, under the renderer lifetime-token
// rules. Exercised directly through handles and through UI::Image sprites, with a renderer that counts calls.
// The cache is process-wide, so counts are compared with what was there when each test began.

using namespace N2Engine;
using Rendering::GpuCache;
using Rendering::Texture;
using Renderer::Common::RenderState;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using TextureTestSupport::CountingRenderer;
using TextureTestSupport::RendererCounts;

namespace
{
    std::shared_ptr<Texture> MakeTexture(const std::uint8_t shade = 128)
    {
        TextureOptions options;
        options.filter = TextureFilter::Nearest;
        return Texture::Create(2, 1, std::vector<std::uint8_t>{shade, 0, 0, 255, 0, shade, 0, 255}, options);
    }

    /// An Image showing `sprite` on its own object (kept alive by the returned pointer)
    struct ImageObject
    {
        GameObject::Ptr gameObject;
        UI::Image *image = nullptr;
    };

    ImageObject MakeImage(const std::string &name, const std::shared_ptr<Texture> &sprite)
    {
        ImageObject result;
        result.gameObject = GameObject::Create(name);
        result.image = result.gameObject->AddComponent<UI::Image>();
        result.image->SetSprite(sprite);
        return result;
    }

    void Draw(UI::Image &image, Renderer::Common::IRenderer &renderer)
    {
        image.RenderUI(&renderer, UI::Rect{0.0f, 0.0f, 10.0f, 10.0f}, RenderState{});
    }

    /// The texture the renderer's last draw used
    Renderer::Common::ITexture *LastDrawnTexture(const CountingRenderer &renderer)
    {
        return renderer.drawnMaterials.empty() ? nullptr : CountingRenderer::TextureOf(renderer.drawnMaterials.back());
    }
}

// ============================================================================
// Handles
// ============================================================================

TEST(GpuCacheTest, OneTexturePerRendererAndAssetSharedByEveryHandle)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    CountingRenderer renderer;
    const auto texture = MakeTexture();

    GpuCache::Handle first = GpuCache::AcquireTexture(renderer, texture);
    GpuCache::Handle second = GpuCache::AcquireTexture(renderer, texture);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.GetTexture(), second.GetTexture());
    EXPECT_EQ(first.GetSource(), texture.get());
    EXPECT_EQ(first.GetRenderer(), &renderer);
    EXPECT_TRUE(first.Holds(&renderer));
    EXPECT_EQ(renderer.Counts().createdTextures, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 2u);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 1);

    // Uploaded as RGBA with the texture's own options
    ASSERT_EQ(renderer.textures.size(), 1u);
    EXPECT_EQ(renderer.Counts().textureChannels[0], 4u);
    EXPECT_EQ(renderer.Counts().textureOptions[0], texture->GetTextureOptions());
    EXPECT_EQ(renderer.textures[0]->data, texture->GetPixels());

    first.Release(true);
    EXPECT_FALSE(first);
    EXPECT_EQ(renderer.Counts().destroyedTextures, 0) << "the other handle still uses it";
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 1u);

    second.Release(true);
    EXPECT_EQ(renderer.Counts().destroyedTextures, 1) << "the last release destroys it";
    EXPECT_TRUE(renderer.textures.empty());
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCacheTest, TheEntryKeepsItsAssetAlive)
{
    CountingRenderer renderer;
    auto texture = MakeTexture();
    const std::weak_ptr<Texture> watch = texture;
    GpuCache::Handle handle = GpuCache::AcquireTexture(renderer, texture);
    texture.reset();
    EXPECT_FALSE(watch.expired()) << "its address is half the key, so it can't be reused meanwhile";
    handle.Release(true);
    EXPECT_TRUE(watch.expired());
}

TEST(GpuCacheTest, ADestructorReleasesWithoutCallingTheRenderer)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    CountingRenderer renderer;
    const auto texture = MakeTexture();
    {
        GpuCache::Handle handle = GpuCache::AcquireTexture(renderer, texture);
        ASSERT_TRUE(handle);
    }
    EXPECT_EQ(renderer.Counts().destroyedTextures, 0) << "the renderer frees it itself when it shuts down";
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline) << "but the entry is gone";
}

TEST(GpuCacheTest, HandlesMoveAndEmptyHandlesReleaseNothing)
{
    CountingRenderer renderer;
    const auto texture = MakeTexture();

    GpuCache::Handle empty;
    EXPECT_FALSE(empty);
    EXPECT_EQ(empty.GetTexture(), nullptr);
    EXPECT_FALSE(empty.Holds(&renderer));
    empty.Release(true); // harmless

    GpuCache::Handle original = GpuCache::AcquireTexture(renderer, texture);
    GpuCache::Handle moved = std::move(original);
    EXPECT_FALSE(original); // NOLINT(bugprone-use-after-move): a moved-from handle is empty by contract
    ASSERT_TRUE(moved);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 1u) << "moving doesn't add a user";

    GpuCache::Handle assigned = GpuCache::AcquireTexture(renderer, MakeTexture(7));
    EXPECT_EQ(renderer.Counts().createdTextures, 2);
    assigned = std::move(moved); // releases the other texture's only share, which destroys it
    EXPECT_EQ(renderer.Counts().destroyedTextures, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 1u);

    assigned.Release(true);
    EXPECT_EQ(renderer.Counts().destroyedTextures, 2);
}

TEST(GpuCacheTest, NothingToUploadGivesAnEmptyHandleAndNoEntry)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    CountingRenderer renderer;
    EXPECT_FALSE(GpuCache::AcquireTexture(renderer, nullptr));
    EXPECT_FALSE(GpuCache::AcquireTexture(renderer, std::make_shared<Texture>())) << "not loaded";
    EXPECT_FALSE(GpuCache::AcquireFontAtlas(renderer, nullptr));
    EXPECT_EQ(renderer.Counts().createdTextures, 0);

    renderer.failTextures = true;
    EXPECT_FALSE(GpuCache::AcquireTexture(renderer, MakeTexture())) << "the renderer couldn't create it";
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCacheTest, AFontAtlasIsASingleChannelSdfTexture)
{
    CountingRenderer renderer;
    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    GpuCache::Handle atlas = GpuCache::AcquireFontAtlas(renderer, font);
    ASSERT_TRUE(atlas);
    EXPECT_EQ(renderer.Counts().textureChannels.back(), 1u);
    EXPECT_EQ(renderer.Counts().textureOptions.back(), TextureOptions::SdfAtlas());
    EXPECT_EQ(GpuCache::GetUserCount(renderer, font.get()), 1u);
    atlas.Release(true);
    EXPECT_EQ(renderer.Counts().destroyedTextures, 1);
}

TEST(GpuCacheTest, AReleaseAfterTheRendererIsDestroyedNeverCallsIt)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RendererCounts counts; // outlives the renderer
    auto renderer = std::make_unique<CountingRenderer>(counts);
    const auto texture = MakeTexture();
    GpuCache::Handle first = GpuCache::AcquireTexture(*renderer, texture);
    GpuCache::Handle second = GpuCache::AcquireTexture(*renderer, texture);
    ASSERT_TRUE(first);

    renderer.reset();
    EXPECT_FALSE(second.Holds(first.GetRenderer())) << "its renderer is gone";
    first.Release(true);
    second.Release(true);
    EXPECT_EQ(counts.destroyedTextures, 0) << "a destroyed renderer is never called";
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCacheTest, ARendererShutDownIsNotCalledEither)
{
    CountingRenderer renderer;
    const auto texture = MakeTexture();
    GpuCache::Handle handle = GpuCache::AcquireTexture(renderer, texture);
    renderer.Kill(); // Shutdown: its resources are gone, though the object lives on
    handle.Release(true);
    EXPECT_EQ(renderer.Counts().destroyedTextures, 0);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 0u);
}

TEST(GpuCacheTest, ARendererRecreatedAtTheSameAddressGetsItsOwnEntry)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RendererCounts oldCounts;
    RendererCounts newCounts;
    std::optional<CountingRenderer> renderer;
    renderer.emplace(oldCounts);
    const auto texture = MakeTexture();
    GpuCache::Handle stale = GpuCache::AcquireTexture(*renderer, texture);
    renderer.reset();
    renderer.emplace(newCounts); // same storage, so the same address

    EXPECT_EQ(GpuCache::GetUserCount(*renderer, texture.get()), 0u) << "the old entry isn't this renderer's";
    GpuCache::Handle fresh = GpuCache::AcquireTexture(*renderer, texture);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(newCounts.createdTextures, 1) << "a new texture, not the old renderer's";
    EXPECT_EQ(GpuCache::GetUserCount(*renderer, texture.get()), 1u);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 1) << "the stale entry was replaced";

    // The stale handle's release doesn't touch the new entry
    stale.Release(true);
    EXPECT_EQ(GpuCache::GetUserCount(*renderer, texture.get()), 1u);
    EXPECT_EQ(newCounts.destroyedTextures, 0);
    EXPECT_EQ(oldCounts.destroyedTextures, 0);

    fresh.Release(true);
    EXPECT_EQ(newCounts.destroyedTextures, 1);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

// ============================================================================
// Image sprites
// ============================================================================

TEST(ImageSpriteTest, TwoImagesShareOneTextureAndTheLastToLetGoDestroysIt)
{
    CountingRenderer renderer;
    const auto texture = MakeTexture();
    ImageObject a = MakeImage("SpriteA", texture);
    ImageObject b = MakeImage("SpriteB", texture);

    Draw(*a.image, renderer);
    Draw(*b.image, renderer);
    Draw(*a.image, renderer);
    EXPECT_EQ(renderer.Counts().createdTextures, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 2u);
    ASSERT_EQ(renderer.textures.size(), 1u);
    EXPECT_EQ(LastDrawnTexture(renderer), renderer.textures[0].get()) << "the sprite is the material's texture";

    a.image->OnDestroy();
    EXPECT_EQ(renderer.Counts().destroyedTextures, 0);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 1u);
    b.image->OnDestroy();
    EXPECT_EQ(renderer.Counts().destroyedTextures, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 0u);
}

TEST(ImageSpriteTest, EachRendererGetsItsOwnTexture)
{
    CountingRenderer first;
    CountingRenderer second;
    const auto texture = MakeTexture();
    ImageObject a = MakeImage("SpriteTwoA", texture);
    ImageObject b = MakeImage("SpriteTwoB", texture);

    Draw(*a.image, first);
    Draw(*b.image, first);
    Draw(*b.image, second); // b moves: its share on the first renderer is released
    EXPECT_EQ(GpuCache::GetUserCount(first, texture.get()), 1u);
    EXPECT_EQ(GpuCache::GetUserCount(second, texture.get()), 1u);
    EXPECT_EQ(first.Counts().createdTextures, 1);
    EXPECT_EQ(second.Counts().createdTextures, 1);
    EXPECT_EQ(first.Counts().destroyedTextures, 0) << "a still uses it";

    Draw(*a.image, second); // the first renderer's last user leaves, and it still exists, so it is told
    EXPECT_EQ(first.Counts().destroyedTextures, 1);
    EXPECT_EQ(GpuCache::GetUserCount(second, texture.get()), 2u);
    EXPECT_EQ(second.Counts().createdTextures, 1) << "shared on the second renderer too";

    a.image->OnDestroy();
    b.image->OnDestroy();
    EXPECT_EQ(second.Counts().destroyedTextures, 1);
}

TEST(ImageSpriteTest, ARendererRecreatedAtTheSameAddressGetsNewTextures)
{
    RendererCounts oldCounts;
    RendererCounts newCounts;
    std::optional<CountingRenderer> renderer;
    renderer.emplace(oldCounts);
    const auto texture = MakeTexture();
    ImageObject a = MakeImage("SpriteRecreatedA", texture);
    ImageObject b = MakeImage("SpriteRecreatedB", texture);
    Draw(*a.image, *renderer);
    Draw(*b.image, *renderer);

    renderer.reset();
    renderer.emplace(newCounts); // same storage, so the same address
    Draw(*a.image, *renderer);
    Draw(*b.image, *renderer);
    EXPECT_EQ(newCounts.createdTextures, 1) << "one new texture, shared";
    EXPECT_EQ(newCounts.destroyedTextures, 0) << "nothing of the old renderer's is destroyed on the new one";
    EXPECT_EQ(oldCounts.destroyedTextures, 0) << "nor is the old renderer called";
    EXPECT_EQ(GpuCache::GetUserCount(*renderer, texture.get()), 2u);
    ASSERT_EQ(renderer->textures.size(), 1u);
    EXPECT_EQ(LastDrawnTexture(*renderer), renderer->textures[0].get());

    a.image->OnDestroy();
    b.image->OnDestroy();
    EXPECT_EQ(newCounts.destroyedTextures, 1);
}

TEST(ImageSpriteTest, DestroyedAfterItsRendererTheImageNeverCallsIt)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RendererCounts counts;
    auto renderer = std::make_unique<CountingRenderer>(counts);
    ImageObject a = MakeImage("SpriteOrphan", MakeTexture());
    Draw(*a.image, *renderer);
    renderer.reset();

    a.image->OnDestroy();
    EXPECT_EQ(counts.destroyedTextures, 0);
    EXPECT_EQ(counts.destroyedMeshes, 0);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(ImageSpriteTest, ARawTextureWinsOverTheSprite)
{
    CountingRenderer renderer;
    ImageObject a = MakeImage("SpriteRaw", MakeTexture());
    constexpr std::uint8_t white[4] = {255, 255, 255, 255};
    Renderer::Common::ITexture *raw = renderer.CreateTexture(white, 1, 1, 4);
    a.image->SetTexture(raw);

    Draw(*a.image, renderer);
    EXPECT_EQ(LastDrawnTexture(renderer), raw);
    EXPECT_EQ(renderer.Counts().createdTextures, 1) << "the sprite isn't uploaded while a raw texture is set";

    a.image->SetTexture(nullptr);
    Draw(*a.image, renderer);
    EXPECT_EQ(renderer.Counts().createdTextures, 2) << "now the sprite is";
    EXPECT_NE(LastDrawnTexture(renderer), raw);
    EXPECT_NE(LastDrawnTexture(renderer), nullptr);
    a.image->OnDestroy();
}

TEST(ImageSpriteTest, ChangingOrClearingTheSpriteReleasesTheOldShare)
{
    CountingRenderer renderer;
    const auto first = MakeTexture(10);
    const auto second = MakeTexture(20);
    ImageObject a = MakeImage("SpriteSwap", first);
    Draw(*a.image, renderer);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, first.get()), 1u);

    a.image->SetSprite(second);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, first.get()), 0u) << "released when the sprite changes";
    EXPECT_EQ(renderer.Counts().destroyedTextures, 1);
    Draw(*a.image, renderer);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, second.get()), 1u);

    a.image->SetSprite(nullptr);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, second.get()), 0u);
    EXPECT_EQ(renderer.Counts().destroyedTextures, 2);
    Draw(*a.image, renderer);
    EXPECT_EQ(LastDrawnTexture(renderer), nullptr) << "no sprite, no texture";
    a.image->OnDestroy();
}
