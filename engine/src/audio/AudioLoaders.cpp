#include "engine/audio/AudioLoaders.hpp"

#include <fstream>
#include <cstring>
#include <algorithm>
#include <limits>

#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>

#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_mp3.h>

#define DR_FLAC_IMPLEMENTATION
#include <dr_libs/dr_flac.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis/stb_vorbis.c>

#include "engine/Logger.hpp"
#include "engine/io/Vfs.hpp"

namespace N2Engine::Audio
{
    // ============================================================================
    // WAV Loader (using dr_wav)
    // ============================================================================

    AudioData WAVLoader::Load(const std::filesystem::path &path)
    {
        AudioData result;

        // The whole file through the virtual filesystem (a folder or a pack), decoded from memory
        const auto bytes = IO::Vfs::Instance().ReadBytes(path);
        drwav wav;
        if (!bytes || !drwav_init_memory(&wav, bytes->data(), bytes->size(), nullptr))
        {
            Logger::Error(std::format("Failed to open WAV file: {}", path.string()));
            return result;
        }

        // Allocate buffer for all samples
        std::size_t totalSampleCount = wav.totalPCMFrameCount * wav.channels;
        std::vector<int16_t> samples(totalSampleCount);

        // Read all samples as 16-bit PCM
        drwav_uint64 framesRead = drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, samples.data());

        if (framesRead != wav.totalPCMFrameCount)
        {
            Logger::Warn(std::format("Only read {} of {} frames from WAV file", framesRead, wav.totalPCMFrameCount));
        }

        // Convert to byte buffer
        std::size_t dataSize = framesRead * wav.channels * sizeof(int16_t);
        result.samples.resize(dataSize);
        std::memcpy(result.samples.data(), samples.data(), dataSize);

        result.sampleRate = wav.sampleRate;
        result.channels = wav.channels;
        result.bitsPerSample = 16; // dr_wav converts to 16-bit

        drwav_uninit(&wav);

        Logger::Info(std::format("Loaded WAV: {} ({}Hz, {} channels)",
                                 path.filename().string(),
                                 result.sampleRate,
                                 result.channels));

        return result;
    }

    // ============================================================================
    // OGG Loader (using stb_vorbis)
    // ============================================================================

    AudioData OGGLoader::Load(const std::filesystem::path &path)
    {
        AudioData result;

        int channels = 0, sampleRate = 0;
        short *output = nullptr;
        const auto bytes = IO::Vfs::Instance().ReadBytes(path);
        // stb_vorbis takes an int length
        int numSamples = bytes && bytes->size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())
                             ? stb_vorbis_decode_memory(bytes->data(), static_cast<int>(bytes->size()), &channels, &sampleRate, &output)
                             : -1;

        if (numSamples <= 0)
        {
            Logger::Error(std::format("Failed to load OGG file: {}", path.string()));
            return result;
        }

        // Convert to byte buffer
        std::size_t dataSize = numSamples * channels * sizeof(short);
        result.samples.resize(dataSize);
        std::memcpy(result.samples.data(), output, dataSize);

        result.sampleRate = sampleRate;
        result.channels = channels;
        result.bitsPerSample = 16;

        free(output);

        Logger::Info(std::format("Loaded OGG: {} ({}Hz, {} channels)",
                                 path.filename().string(),
                                 result.sampleRate,
                                 result.channels));

        return result;
    }

    // ============================================================================
    // MP3 Loader (using dr_mp3)
    // ============================================================================

    AudioData MP3Loader::Load(const std::filesystem::path &path)
    {
        AudioData result;

        drmp3_config config;
        drmp3_uint64 totalFrameCount;
        const auto bytes = IO::Vfs::Instance().ReadBytes(path);
        drmp3_int16 *pSampleData = bytes ? drmp3_open_memory_and_read_pcm_frames_s16(
            bytes->data(),
            bytes->size(),
            &config,
            &totalFrameCount,
            nullptr
        ) : nullptr;

        if (pSampleData == nullptr)
        {
            Logger::Error(std::format("Failed to load MP3 file: {}", path.string()));
            return result;
        }

        // Convert to byte buffer
        std::size_t dataSize = totalFrameCount * config.channels * sizeof(drmp3_int16);
        result.samples.resize(dataSize);
        std::memcpy(result.samples.data(), pSampleData, dataSize);

        result.sampleRate = config.sampleRate;
        result.channels = config.channels;
        result.bitsPerSample = 16;

        drmp3_free(pSampleData, nullptr);

        Logger::Info(std::format("Loaded MP3: {} ({}Hz, {} channels)",
                                 path.filename().string(),
                                 result.sampleRate,
                                 result.channels));

        return result;
    }

    // ============================================================================
    // FLAC Loader (using dr_flac)
    // ============================================================================

    AudioData FLACLoader::Load(const std::filesystem::path &path)
    {
        AudioData result;

        unsigned int channels;
        unsigned int sampleRate;
        drflac_uint64 totalFrameCount;
        const auto bytes = IO::Vfs::Instance().ReadBytes(path);
        drflac_int16 *pSampleData = bytes ? drflac_open_memory_and_read_pcm_frames_s16(
            bytes->data(),
            bytes->size(),
            &channels,
            &sampleRate,
            &totalFrameCount,
            nullptr
        ) : nullptr;

        if (pSampleData == nullptr)
        {
            Logger::Error(std::format("Failed to load FLAC file: {}", path.string()));
            return result;
        }

        // Convert to byte buffer
        std::size_t dataSize = totalFrameCount * channels * sizeof(drflac_int16);
        result.samples.resize(dataSize);
        std::memcpy(result.samples.data(), pSampleData, dataSize);

        result.sampleRate = sampleRate;
        result.channels = channels;
        result.bitsPerSample = 16;

        drflac_free(pSampleData, nullptr);

        Logger::Info(std::format("Loaded FLAC: {} ({}Hz, {} channels)",
                                 path.filename().string(),
                                 result.sampleRate,
                                 result.channels));

        return result;
    }
}
