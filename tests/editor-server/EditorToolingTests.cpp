#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTooling.hpp>
#include <gtest/gtest.h>
#include <vector>
using namespace N2Engine::Editor;
namespace
{
    std::vector<std::uint8_t> Tga()
    {
        std::vector<std::uint8_t> data(18, 0);
        data[2] = 2;
        data[12] = 2;
        data[14] = 2;
        data[16] = 32;
        data[17] = 0x28;
        // Top row red/green, bottom row blue/white; TGA stores BGRA.
        const std::array<std::uint8_t, 16> pixels{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
        data.insert(data.end(), pixels.begin(), pixels.end());
        return data;
    }
    std::vector<std::uint8_t> Wav()
    {
        std::vector<std::uint8_t> data;
        auto text = [&](const char *v) { data.insert(data.end(), v, v + 4); };
        auto u16 = [&](std::uint16_t v) {
            data.push_back(v & 255);
            data.push_back(v >> 8);
        };
        auto u32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                data.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        text("RIFF");
        u32(48);
        text("WAVE");
        text("fmt ");
        u32(16);
        u16(1);
        u16(1);
        u32(8000);
        u32(16000);
        u16(2);
        u16(16);
        text("data");
        u32(12);
        for (std::int16_t value : {-32768, -16384, 0, 8192, 16384, 32767})
            u16(static_cast<std::uint16_t>(value));
        return data;
    }
} // namespace
TEST(EditorToolingTest, LuaCompilesWithoutExecutingOrOpeningProjectLibraries)
{
    for (auto source :
         {"", "while true do end", "error('must not run')", "local x <const> = 1; return x", "os.remove('never')"})
    {
        const auto result = Tooling::CheckLuaSyntax(source, "res://scripts/probe.lua");
        ASSERT_TRUE(result);
        EXPECT_EQ(result->at("ok"), true);
        EXPECT_EQ(result->at("line"), 0);
    }
    const auto error = Tooling::CheckLuaSyntax("local x = 1\nlocal = 2", "C:/private/source.lua");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->at("ok"), false);
    EXPECT_EQ(error->at("line"), 2);
    EXPECT_FALSE(error->contains("column"));
    EXPECT_EQ(error->dump().find("private"), std::string::npos);
    const std::string binary("\x1bLua", 4);
    const auto refused = Tooling::CheckLuaSyntax(binary, "binary.lua");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->at("ok"), false);
}
TEST(EditorToolingTest, LuaInputCapsAreCheckedBeforeCompiling)
{
    EXPECT_FALSE(Tooling::CheckLuaSyntax(std::string(Tooling::MaxSyntaxBytes + 1, ' '), "script.lua"));
    EXPECT_FALSE(Tooling::CheckLuaSyntax("return 1", std::string(257, 'a')));
    Protocol::BufferWriter payload;
    payload.WriteU32(static_cast<std::uint32_t>(Tooling::MaxSyntaxBytes + 1));
    Protocol::BufferReader reader(payload.Data());
    EXPECT_THROW((void)Protocol::CheckLuaSyntaxCmd::Deserialize(reader), std::invalid_argument);
}
TEST(EditorToolingTest, TextureIsTopFirstBoxFilteredAndNeverUpscaled)
{
    auto full = Tooling::PreviewImage(Tga(), 256);
    ASSERT_TRUE(full);
    EXPECT_EQ(full->info["width"], 2);
    EXPECT_EQ(full->info["height"], 2);
    EXPECT_EQ(full->pixels,
              (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}));
    auto small = Tooling::PreviewImage(Tga(), 1);
    ASSERT_TRUE(small);
    EXPECT_EQ(small->pixels, (std::vector<std::uint8_t>{128, 128, 128, 255}));
    EXPECT_FALSE(Tooling::PreviewImage(Tga(), 0));
    EXPECT_FALSE(Tooling::PreviewImage(Tga(), 257));
    auto large = Tga();
    large[12] = 0;
    large[13] = 64;
    large[14] = 0;
    large[15] = 64;
    EXPECT_FALSE(Tooling::PreviewImage(large, 256));
}
TEST(EditorToolingTest, WaveformContainsBoundedPeaksWithoutAnAudioDevice)
{
    auto result = Tooling::PreviewAudio(Wav(), ".wav", 2);
    ASSERT_TRUE(result) << result.error();
    EXPECT_TRUE(result->pixels.empty());
    EXPECT_EQ(result->info["kind"], "audio");
    EXPECT_EQ(result->info["channels"], 1);
    EXPECT_EQ(result->info["sampleRate"], 8000);
    EXPECT_DOUBLE_EQ(result->info["durationMs"].get<double>(), 0.75);
    const auto &peaks = result->info["peaks"];
    ASSERT_EQ(peaks.size(), 4u);
    EXPECT_DOUBLE_EQ(peaks[0]["min"].get<double>(), -1.0);
    EXPECT_DOUBLE_EQ(peaks[1]["max"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(peaks[3]["min"].get<double>(), 0.5);
    EXPECT_FALSE(Tooling::PreviewAudio({}, ".wav", 256));
    for (auto extension : {".mp3", ".flac", ".ogg", ".bad"})
        EXPECT_FALSE(Tooling::PreviewAudio(Tga(), extension, 256));
}
TEST(EditorToolingTest, EveryAdvertisedAudioFormatDecodesWithoutADevice)
{
    for (const auto extension : {".wav", ".mp3", ".flac", ".ogg"})
    {
        const auto file = std::filesystem::path(N2_EDITOR_TOOLING_FIXTURES) / (std::string("tone") + extension);
        const auto preview = Tooling::BuildAssetPreview(file, "AudioClip", 256);
        ASSERT_TRUE(preview) << extension << ": " << preview.error();
        EXPECT_TRUE(preview->pixels.empty());
        EXPECT_EQ(preview->info.at("channels"), 1);
        EXPECT_EQ(preview->info.at("sampleRate"), 8000);
        EXPECT_NEAR(preview->info.at("durationMs").get<double>(), 100.0, 0.2) << extension;
        const auto &peaks = preview->info.at("peaks");
        ASSERT_FALSE(peaks.empty());
        EXPECT_LE(peaks.size(), 512u);
        bool audible = false;
        for (const auto &peak : peaks)
        {
            const auto low = peak.at("min").get<double>(), high = peak.at("max").get<double>();
            EXPECT_GE(low, -1.0); EXPECT_LT(high, 1.0); EXPECT_LE(low, high);
            audible |= low < -0.01 || high > 0.01;
        }
        EXPECT_TRUE(audible);
    }
}

TEST(EditorToolingTest, SyntaxCommandIsReadOnlyAndReturnsDiagnosticJson)
{
    EditorServer server;
    server.SetReadOnly(true);
    Protocol::BufferWriter payload;
    payload.WriteString("while true do end");
    payload.WriteString("loop.lua");
    const auto response =
        server.ExecuteCommand(static_cast<std::uint8_t>(CommandType::CheckLuaSyntax), payload.Release());
    Protocol::BufferReader frame(response);
    EXPECT_EQ(frame.ReadU8(), static_cast<std::uint8_t>(ResponseType::LuaSyntax));
    const auto length = frame.ReadU32();
    Protocol::BufferReader body(frame.ReadBytes(length));
    EXPECT_EQ(Protocol::ReadJson(body)["ok"], true);
}

TEST(EditorToolingTest, ToolingIsAdvertisedAndRejectsMalformedOrTrailingPayloads)
{
    const auto capabilities = EditorServer::Capabilities();
    for (const auto capability : {"editorTooling", "luaSyntax", "assetPreviews"})
        EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), capability), capabilities.end());
    EditorServer server;
    const auto expectError = [&](CommandType command, const std::vector<std::uint8_t> &payload) {
        const auto response = server.ExecuteCommand(static_cast<std::uint8_t>(command), payload);
        ASSERT_FALSE(response.empty());
        EXPECT_EQ(response[0], static_cast<std::uint8_t>(ResponseType::Error));
    };
    expectError(CommandType::CheckLuaSyntax, {});
    Protocol::BufferWriter source;
    source.WriteString("return 1");
    source.WriteString("source.lua");
    source.WriteU8(0);
    expectError(CommandType::CheckLuaSyntax, source.Release());
    expectError(CommandType::SetEditorViewOptions, {});
    expectError(CommandType::GetAssetPreview, {});
    Protocol::BufferWriter preview;
    preview.WriteString("res://file.png");
    preview.WriteU32(256);
    expectError(CommandType::GetAssetPreview, preview.Release()); // No project.
}
