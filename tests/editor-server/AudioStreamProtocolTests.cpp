#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/audio/AudioSystem.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;

namespace
{
    struct DecodedAudio
    {
        uint8_t responseType = 0xEE;
        uint32_t payloadSize = 0;
        size_t payloadBytesRemaining = 0;
        uint32_t sampleRate = 0;
        uint32_t channels = 0;
        std::string sampleFormat;
        uint32_t frameCount = 0;
        uint32_t droppedFrames = 0;
        std::vector<uint8_t> samples;
    };

    // Decodes an AudioSamples response the way a client would, following protocol.json: the samples run
    // to the end of the payload, as FrameData's pixels do
    DecodedAudio Decode(std::span<const uint8_t> frame)
    {
        BufferReader r(frame);
        DecodedAudio out;
        out.responseType = r.ReadU8();
        out.payloadSize = r.ReadU32();
        out.payloadBytesRemaining = r.Remaining();
        if (out.responseType != static_cast<uint8_t>(ResponseType::AudioSamples))
        {
            return out;
        }
        out.sampleRate = r.ReadU32();
        out.channels = r.ReadU32();
        out.sampleFormat = r.ReadString();
        out.frameCount = r.ReadU32();
        out.droppedFrames = r.ReadU32();
        const auto samples = r.ReadBytes(r.Remaining());
        out.samples.assign(samples.begin(), samples.end());
        return out;
    }

    uint8_t ResponseTypeOf(const std::vector<uint8_t> &frame)
    {
        return frame.empty() ? uint8_t{0xEE} : frame[0];
    }

    constexpr auto GetAudioCommand = static_cast<uint8_t>(CommandType::GetAudio);
}

TEST(AudioStreamProtocolTest, UsesProtocolIds)
{
    EXPECT_EQ(static_cast<uint8_t>(CommandType::GetAudio), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(ResponseType::AudioSamples), 0x0A);
}

TEST(AudioStreamProtocolTest, AudioSamplesRoundTrip)
{
    const std::vector<uint8_t> samples = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    BufferWriter w;
    WriteAudioSamples(w, 48000, 2, "float32", 2, 7, samples);

    const DecodedAudio decoded = Decode(w.Data());
    EXPECT_EQ(decoded.responseType, static_cast<uint8_t>(ResponseType::AudioSamples));
    EXPECT_EQ(decoded.payloadSize, decoded.payloadBytesRemaining);
    EXPECT_EQ(decoded.sampleRate, 48000u);
    EXPECT_EQ(decoded.channels, 2u);
    EXPECT_EQ(decoded.sampleFormat, "float32");
    EXPECT_EQ(decoded.frameCount, 2u);
    EXPECT_EQ(decoded.droppedFrames, 7u);
    EXPECT_EQ(decoded.samples, samples);
}

TEST(AudioStreamProtocolTest, EmptyAudioSamplesRoundTrip)
{
    BufferWriter w;
    WriteAudioSamples(w, 48000, 2, "int16", 0, 0, {});

    const DecodedAudio decoded = Decode(w.Data());
    EXPECT_EQ(decoded.payloadSize, decoded.payloadBytesRemaining);
    EXPECT_EQ(decoded.sampleFormat, "int16");
    EXPECT_EQ(decoded.frameCount, 0u);
    EXPECT_TRUE(decoded.samples.empty());
}

TEST(AudioStreamProtocolTest, GetAudioWithoutALoopbackDeviceIsAnError)
{
    Audio::AudioSystem::Instance().Shutdown();

    EditorServer server;
    EXPECT_EQ(ResponseTypeOf(server.ExecuteCommand(GetAudioCommand, {})), static_cast<uint8_t>(ResponseType::Error));
    server.UpdateAudio(); // no-op without a loopback device
    server.UpdateAudio();
}

class AudioStreamServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Audio::AudioSystem::Instance().Shutdown();
        if (!Audio::AudioSystem::IsLoopbackSupported())
        {
            GTEST_SKIP() << "This OpenAL build has no ALC_SOFT_loopback extension; audio streaming can't be tested";
        }
        ASSERT_TRUE(Audio::AudioSystem::Instance().Initialize(Audio::AudioOutput::Loopback));
    }

    void TearDown() override
    {
        Audio::AudioSystem::Instance().Shutdown();
    }

    EditorServer _server;
};

TEST_F(AudioStreamServerTest, GetAudioReturnsEverythingMixedSinceTheLastCall)
{
    auto &audio = Audio::AudioSystem::Instance();
    const Audio::LoopbackFormat &format = audio.GetLoopbackFormat();

    audio.AdvanceStream(0.1); // 100 ms of engine time: 4800 frames

    const DecodedAudio first = Decode(_server.ExecuteCommand(GetAudioCommand, {}));
    ASSERT_EQ(first.responseType, static_cast<uint8_t>(ResponseType::AudioSamples));
    EXPECT_EQ(first.payloadSize, first.payloadBytesRemaining);
    EXPECT_EQ(first.sampleRate, 48000u);
    EXPECT_EQ(first.channels, 2u);
    EXPECT_EQ(first.sampleFormat, std::string{Audio::ToString(format.sampleFormat)});
    EXPECT_EQ(first.frameCount, 4800u);
    EXPECT_EQ(first.droppedFrames, 0u);
    EXPECT_EQ(first.samples.size(), static_cast<size_t>(first.frameCount) * format.BytesPerFrame());

    // Drained: the next call has only what was mixed since
    const DecodedAudio second = Decode(_server.ExecuteCommand(GetAudioCommand, {}));
    ASSERT_EQ(second.responseType, static_cast<uint8_t>(ResponseType::AudioSamples));
    EXPECT_EQ(second.frameCount, 0u);
    EXPECT_TRUE(second.samples.empty());
}

TEST_F(AudioStreamServerTest, GetAudioAfterALongGapReturnsAtMostTheBufferAndCountsTheRest)
{
    auto &audio = Audio::AudioSystem::Instance();
    for (int i = 0; i < 120; ++i) // two seconds with no client reading
    {
        audio.AdvanceStream(1.0 / 60.0);
    }

    const DecodedAudio decoded = Decode(_server.ExecuteCommand(GetAudioCommand, {}));
    ASSERT_EQ(decoded.responseType, static_cast<uint8_t>(ResponseType::AudioSamples));
    EXPECT_EQ(decoded.frameCount, audio.GetStreamCapacityFrames());
    EXPECT_NEAR(static_cast<double>(decoded.droppedFrames), 96000.0 - audio.GetStreamCapacityFrames(), 1.0);
}
