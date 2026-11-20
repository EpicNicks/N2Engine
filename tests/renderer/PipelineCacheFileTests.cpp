#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <renderer/common/PipelineCache.hpp>

// The Vulkan pipeline cache's file (user shaders, stage 7): the header check that decides whether saved data is handed back to a driver,
// the size cap and the atomic save. The renderer itself is exercised by the Vulkan tests (every pipeline is made through the cache).
namespace
{
    namespace Cache = N2Engine::Renderer::Common::PipelineCacheFile;

    std::vector<std::uint8_t> Header(const std::uint32_t vendor, const std::uint32_t device, const std::uint8_t (&uuid)[16],
                                     const std::uint32_t headerSize = 32, const std::uint32_t version = 1, const std::size_t total = 48)
    {
        std::vector<std::uint8_t> bytes(total, 0xAB);
        const auto put = [&](const std::size_t at, const std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
            {
                bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        };
        put(0, headerSize);
        put(4, version);
        put(8, vendor);
        put(12, device);
        std::memcpy(bytes.data() + 16, uuid, 16);
        return bytes;
    }

    constexpr std::uint8_t kUuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
}

TEST(PipelineCacheFileTest, TheHeaderMustNameThisDeviceAndDriver)
{
    const auto good = Header(0x10DE, 0x2684, kUuid);
    EXPECT_TRUE(Cache::HeaderMatches(good.data(), good.size(), 0x10DE, 0x2684, kUuid));
    EXPECT_FALSE(Cache::HeaderMatches(good.data(), good.size(), 0x1002, 0x2684, kUuid)) << "another vendor";
    EXPECT_FALSE(Cache::HeaderMatches(good.data(), good.size(), 0x10DE, 0x2204, kUuid)) << "another device";
    constexpr std::uint8_t otherDriver[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};
    EXPECT_FALSE(Cache::HeaderMatches(good.data(), good.size(), 0x10DE, 0x2684, otherDriver)) << "another driver build";
    const auto oldVersion = Header(0x10DE, 0x2684, kUuid, 32, 2);
    EXPECT_FALSE(Cache::HeaderMatches(oldVersion.data(), oldVersion.size(), 0x10DE, 0x2684, kUuid));
    const auto shortHeader = Header(0x10DE, 0x2684, kUuid, 16);
    EXPECT_FALSE(Cache::HeaderMatches(shortHeader.data(), shortHeader.size(), 0x10DE, 0x2684, kUuid));
    EXPECT_FALSE(Cache::HeaderMatches(good.data(), 31, 0x10DE, 0x2684, kUuid)) << "truncated";
    EXPECT_FALSE(Cache::HeaderMatches(nullptr, 0, 0x10DE, 0x2684, kUuid));
}

TEST(PipelineCacheFileTest, SaveAndLoadRoundTripAndRefuseForeignOrOversizedFiles)
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "n2-pipeline-cache-test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    const std::filesystem::path file = folder / "sub" / Cache::FileName(0x10DE, 0x2684);
    EXPECT_EQ(Cache::FileName(0x10DE, 0x2684), "vk-pipelines-000010de-00002684.bin");

    const auto data = Header(0x10DE, 0x2684, kUuid, 32, 1, 200);
    ASSERT_TRUE(Cache::Save(file, data.data(), data.size())) << "the folder is made";
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp")) << "written through a temporary file, renamed";
    const auto loaded = Cache::Load(file, 0x10DE, 0x2684, kUuid);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, data);
    EXPECT_FALSE(Cache::Load(file, 0x1002, 0x2684, kUuid).has_value()) << "saved by another GPU";
    EXPECT_FALSE(Cache::Load(folder / "missing.bin", 0x10DE, 0x2684, kUuid).has_value());

    EXPECT_FALSE(Cache::Save(file, data.data(), 0)) << "an empty cache is not saved";
    std::ofstream(folder / "garbage.bin", std::ios::binary) << "not a pipeline cache at all, but longer than a header would be....";
    EXPECT_FALSE(Cache::Load(folder / "garbage.bin", 0x10DE, 0x2684, kUuid).has_value());
    std::filesystem::remove_all(folder, error);
}

TEST(PipelineCacheFileTest, TheDirectoryIsAdvisorySettingsState)
{
    const auto before = Cache::GetDirectory();
    Cache::SetDirectory("some/folder");
    EXPECT_EQ(Cache::GetDirectory(), std::filesystem::path("some/folder"));
    Cache::SetDirectory(before);
}
