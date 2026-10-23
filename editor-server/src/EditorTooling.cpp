#include "editor-server/EditorTooling.hpp"

#include <algorithm>
#include <array>
#include <assetimport/ImageDecoder.hpp>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <dr_libs/dr_flac.h>
#include <dr_libs/dr_mp3.h>
#include <dr_libs/dr_wav.h>
#include <fstream>
#include <functional>
#include <memory>
#include <utility>
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis/stb_vorbis.c>
extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}

namespace N2Engine::Editor::Tooling
{
    namespace
    {
        struct LuaMemory
        {
            std::size_t used = 0;
            static void *Allocate(void *context, void *pointer, std::size_t oldSize, std::size_t newSize)
            {
                auto &memory = *static_cast<LuaMemory *>(context);
                if (!pointer)
                    oldSize = 0; // Lua passes a type tag in osize for a new object.
                if (newSize == 0)
                {
                    std::free(pointer);
                    memory.used -= oldSize;
                    return nullptr;
                }
                constexpr std::size_t limit = 64 * 1024 * 1024;
                if (oldSize > memory.used || newSize > limit - (memory.used - oldSize))
                    return nullptr;
                void *next = std::realloc(pointer, newSize);
                if (next)
                    memory.used = memory.used - oldSize + newSize;
                return next;
            }
        };
        bool ValidSize(std::uint32_t size)
        {
            return size >= 1 && size <= MaxPreviewSize;
        }

        template <class Read>
        std::expected<AssetPreview, std::string> Waveform(unsigned channels, unsigned sampleRate,
                                                          std::uint64_t knownFrames, std::uint32_t maxSize, Read read)
        {
            if (channels < 1 || channels > 8 || sampleRate < 1 || sampleRate > 384'000)
                return std::unexpected("Audio format is outside the preview limits");
            if (knownFrames > MaxPreviewSampleValues / channels)
                return std::unexpected("Audio exceeds the preview sample budget");
            std::vector<std::int16_t> samples;
            if (knownFrames)
                samples.reserve(static_cast<std::size_t>(knownFrames) * channels);
            std::array<std::int16_t, 4096 * 8> block{};
            while (true)
            {
                const auto count = static_cast<std::uint64_t>(read(4096, block.data()));
                if (!count)
                    break;
                if (count > 4096 || count * channels > MaxPreviewSampleValues - samples.size())
                    return std::unexpected("Audio exceeds the preview sample budget");
                samples.insert(samples.end(), block.begin(),
                               block.begin() + static_cast<std::size_t>(count) * channels);
            }
            const std::size_t frames = samples.size() / channels;
            if (!frames || (knownFrames && frames != knownFrames))
                return std::unexpected("Audio is empty or truncated");
            const std::size_t bins = std::min<std::size_t>(maxSize * 2, frames);
            nlohmann::json peaks = nlohmann::json::array();
            for (std::size_t bin = 0; bin < bins; ++bin)
            {
                const auto start = bin * frames / bins, end = (bin + 1) * frames / bins;
                std::int16_t low = 32767, high = -32768;
                for (std::size_t i = start * channels; i < end * channels; ++i)
                {
                    low = std::min(low, samples[i]);
                    high = std::max(high, samples[i]);
                }
                peaks.push_back(
                    {{"min", static_cast<double>(low) / 32768.0}, {"max", static_cast<double>(high) / 32768.0}});
            }
            return AssetPreview{{{"kind", "audio"},
                                 {"durationMs", static_cast<double>(frames) * 1000 / sampleRate},
                                 {"channels", channels},
                                 {"sampleRate", sampleRate},
                                 {"peaks", std::move(peaks)}},
                                {}};
        }
    } // namespace

    std::expected<nlohmann::json, std::string> CheckLuaSyntax(std::string_view text, std::string_view chunkName)
    {
        if (text.size() > MaxSyntaxBytes)
            return std::unexpected("Lua syntax input exceeds 4 MiB");
        if (chunkName.size() > 256)
            return std::unexpected("Lua chunk name exceeds 256 bytes");
        // A short diagnostic label, never a filesystem path or a Lua '@' file-name chunk.
        const auto separator = chunkName.find_last_of("/\\");
        auto base = separator == std::string_view::npos ? chunkName : chunkName.substr(separator + 1);
        std::string label(base.substr(0, 48));
        if (label.empty())
            label = "script";
        for (char &c : label)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-'))
                c = '_';
        const std::string name = "=" + label;
        LuaMemory memory;
        std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(&LuaMemory::Allocate, &memory), &lua_close);
        if (!state)
            return std::unexpected("Could not allocate the Lua syntax checker");
        const int status = luaL_loadbufferx(state.get(), text.data(), text.size(), name.c_str(), "t");
        if (status == LUA_ERRMEM)
            return std::unexpected("Lua syntax checker memory budget exceeded");
        if (status == LUA_OK)
            return nlohmann::json{{"ok", true}, {"line", 0}, {"message", ""}};
        std::size_t length = 0;
        const char *message =
            lua_type(state.get(), -1) == LUA_TSTRING ? lua_tolstring(state.get(), -1, &length) : nullptr;
        std::string reason = message ? std::string(message, length) : "Invalid Lua source";
        std::uint32_t line = 0;
        const std::string prefix = label + ":";
        if (reason.starts_with(prefix))
        {
            const auto end = reason.find(':', prefix.size());
            if (end != std::string::npos)
            {
                const auto first = reason.data() + prefix.size(), last = reason.data() + end;
                const auto parsed = std::from_chars(first, last, line);
                if (parsed.ec == std::errc{} && parsed.ptr == last)
                {
                    reason.erase(0, end + 1);
                    if (reason.starts_with(' '))
                        reason.erase(0, 1);
                }
            }
        }
        return nlohmann::json{{"ok", false}, {"line", line}, {"message", std::move(reason)}};
    }

    std::expected<AssetPreview, std::string> PreviewImage(std::span<const std::uint8_t> encoded, std::uint32_t maxSize)
    {
        if (!ValidSize(maxSize))
            return std::unexpected("Preview size must be between 1 and 256");
        if (encoded.size() > MaxPreviewFileBytes)
            return std::unexpected("Asset exceeds the preview file budget");
        AssetImport::ImageDecodeOptions options;
        options.flipY = false;
        options.maxDecodedBytes = 64 * 1024 * 1024;
        auto image = AssetImport::DecodeImage(encoded, options);
        if (!image)
            return std::unexpected("Could not decode texture within the preview limits");
        const auto longest = std::max(image->width, image->height);
        const auto width =
            longest > maxSize
                ? std::max(1u, static_cast<std::uint32_t>(std::uint64_t(image->width) * maxSize / longest))
                : image->width;
        const auto height =
            longest > maxSize
                ? std::max(1u, static_cast<std::uint32_t>(std::uint64_t(image->height) * maxSize / longest))
                : image->height;
        AssetPreview preview{{{"kind", "texture"},
                              {"width", width},
                              {"height", height},
                              {"sourceWidth", image->width},
                              {"sourceHeight", image->height}},
                             std::vector<std::uint8_t>(width * height * 4)};
        // Non-overlapping source boxes: every decoded pixel is visited at most once, output is top-row-first.
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const auto x0 = std::uint64_t(x) * image->width / width,
                           x1 = std::uint64_t(x + 1) * image->width / width;
                const auto y0 = std::uint64_t(y) * image->height / height,
                           y1 = std::uint64_t(y + 1) * image->height / height;
                std::array<std::uint64_t, 4> sum{};
                for (auto sy = y0; sy < y1; ++sy)
                    for (auto sx = x0; sx < x1; ++sx)
                        for (std::size_t c = 0; c < 4; ++c)
                            sum[c] += image->pixels[(sy * image->width + sx) * 4 + c];
                const auto count = (x1 - x0) * (y1 - y0);
                for (std::size_t c = 0; c < 4; ++c)
                    preview.pixels[(std::size_t(y) * width + x) * 4 + c] =
                        static_cast<std::uint8_t>((sum[c] + count / 2) / count);
            }
        return preview;
    }

    std::expected<AssetPreview, std::string> PreviewAudio(std::span<const std::uint8_t> data,
                                                          std::string_view extension, std::uint32_t size)
    {
        if (!ValidSize(size))
            return std::unexpected("Preview size must be between 1 and 256");
        if (data.empty() || data.size() > MaxPreviewFileBytes)
            return std::unexpected("Audio is empty or exceeds the preview file budget");
        if (extension == ".wav")
        {
            drwav decoder{};
            if (!drwav_init_memory(&decoder, data.data(), data.size(), nullptr))
                return std::unexpected("Could not decode WAV audio");
            struct Close
            {
                drwav *p;
                ~Close()
                {
                    drwav_uninit(p);
                }
            } close{&decoder};
            return Waveform(decoder.channels, decoder.sampleRate, decoder.totalPCMFrameCount, size,
                            [&](auto count, auto *out) { return drwav_read_pcm_frames_s16(&decoder, count, out); });
        }
        if (extension == ".mp3")
        {
            drmp3 decoder{};
            if (!drmp3_init_memory(&decoder, data.data(), data.size(), nullptr))
                return std::unexpected("Could not decode MP3 audio");
            struct Close
            {
                drmp3 *p;
                ~Close()
                {
                    drmp3_uninit(p);
                }
            } close{&decoder};
            return Waveform(decoder.channels, decoder.sampleRate, 0, size,
                            [&](auto count, auto *out) { return drmp3_read_pcm_frames_s16(&decoder, count, out); });
        }
        if (extension == ".flac")
        {
            std::unique_ptr<drflac, std::function<void(drflac *)>> decoder(
                drflac_open_memory(data.data(), data.size(), nullptr), [](drflac *p) { drflac_close(p); });
            if (!decoder)
                return std::unexpected("Could not decode FLAC audio");
            return Waveform(
                decoder->channels, decoder->sampleRate, decoder->totalPCMFrameCount, size,
                [&](auto count, auto *out) { return drflac_read_pcm_frames_s16(decoder.get(), count, out); });
        }
        if (extension == ".ogg")
        {
            int error = 0;
            std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> decoder(
                stb_vorbis_open_memory(data.data(), static_cast<int>(data.size()), &error, nullptr), &stb_vorbis_close);
            if (!decoder)
                return std::unexpected("Could not decode Vorbis audio");
            const auto info = stb_vorbis_get_info(decoder.get());
            return Waveform(info.channels, info.sample_rate, stb_vorbis_stream_length_in_samples(decoder.get()), size,
                            [&](auto count, auto *out) {
                                return stb_vorbis_get_samples_short_interleaved(
                                    decoder.get(), info.channels, out, static_cast<int>(count * info.channels));
                            });
        }
        return std::unexpected("This audio format has no preview");
    }

    std::expected<AssetPreview, std::string> BuildAssetPreview(const std::filesystem::path &file, std::string_view type,
                                                               std::uint32_t size)
    {
        if (!ValidSize(size))
            return std::unexpected("Preview size must be between 1 and 256");
        if (type != "Texture" && type != "AudioClip")
            return std::unexpected("This asset type has no preview");
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        if (!stream)
            return std::unexpected("Could not read preview asset");
        const auto end = stream.tellg();
        if (end < 0 || static_cast<std::uint64_t>(end) > MaxPreviewFileBytes)
            return std::unexpected("Asset exceeds the preview file budget");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        stream.seekg(0);
        if (!bytes.empty() &&
            !stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return std::unexpected("Could not read preview asset");
        if (type == "Texture")
            return PreviewImage(bytes, size);
        auto extension = file.extension().string();
        for (auto &c : extension)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return PreviewAudio(bytes, extension, size);
    }
} // namespace N2Engine::Editor::Tooling
