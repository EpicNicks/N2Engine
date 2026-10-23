#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/n2net/ByteIO.hpp"

// The world messages' codec (netcode stage B1): every message round-trips, every length is checked before anything is
// taken, a malformed message is rejected with a reason and no effect, a client cannot say a server's words, and no
// mutation of a valid message crashes the decoder (what still decodes re-encodes to the same bytes).

using namespace N2Engine::Network;
using N2Engine::N2Net::ByteWriter;

namespace
{
    Fingerprint SampleFingerprint()
    {
        Fingerprint fingerprint;
        fingerprint.engineVersion = "1.2.3";
        fingerprint.sceneHash = 0x0123456789ABCDEFull;
        fingerprint.registryHash = 0xFEDCBA9876543210ull;
        fingerprint.scenePath = "res://scenes/main.scene";
        fingerprint.sceneObjectCount = 7;
        fingerprint.prefabCount = 3;
        return fingerprint;
    }

    Fingerprint LongestFingerprint()
    {
        Fingerprint fingerprint = SampleFingerprint();
        fingerprint.engineVersion = std::string(kMaxEngineVersionLength, 'v');
        fingerprint.scenePath = std::string(kMaxScenePathLength, 'p');
        fingerprint.sceneObjectCount = kMaxNetworkObjects;
        fingerprint.prefabCount = kMaxPrefabs;
        return fingerprint;
    }

    ServerHelloMessage SampleHello()
    {
        ServerHelloMessage hello;
        hello.fingerprint = SampleFingerprint();
        hello.clientId = 42;
        return hello;
    }

    ClientAcceptMessage SampleAccept()
    {
        ClientAcceptMessage accept;
        accept.fingerprint = SampleFingerprint();
        return accept;
    }

    SpawnMessage SampleSpawn()
    {
        SpawnMessage spawn;
        spawn.id = NetworkId{4096};
        spawn.prefabIndex = 17;
        spawn.owner = 3;
        return spawn;
    }

    std::vector<std::vector<std::uint8_t>> ValidServerMessages()
    {
        return {Encode(SampleHello()), Encode(SampleSpawn()), Encode(DespawnMessage{NetworkId{9}}),
                Encode(WorldReadyMessage{5, 2})};
    }

    std::vector<std::vector<std::uint8_t>> ValidClientMessages()
    {
        return {Encode(SampleAccept()), Encode(ClientRejectMessage{JoinRefusal::SceneContent})};
    }

    std::vector<std::uint8_t> Reencode(const ServerMessage &message)
    {
        return std::visit([](const auto &value) { return Encode(value); }, message);
    }

    std::vector<std::uint8_t> Reencode(const ClientMessage &message)
    {
        return std::visit([](const auto &value) { return Encode(value); }, message);
    }

    /// A hello built by hand, so a test can put anything in the fields
    std::vector<std::uint8_t> HandmadeHello(const std::uint16_t version, const std::uint8_t engineLength,
                                            const std::vector<std::uint8_t> &engineBytes,
                                            const std::uint8_t pathLength, const std::vector<std::uint8_t> &pathBytes,
                                            const std::uint32_t objectCount, const std::uint16_t prefabCount,
                                            const std::uint32_t clientId)
    {
        std::array<std::uint8_t, 600> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(static_cast<std::uint8_t>(MessageType::ServerHello));
        writer.U16(version);
        writer.U8(engineLength);
        writer.Bytes(engineBytes);
        writer.U32(0);
        writer.U32(0); // scene hash
        writer.U32(0);
        writer.U32(0); // registry hash
        writer.U8(pathLength);
        writer.Bytes(pathBytes);
        writer.U32(objectCount);
        writer.U16(prefabCount);
        writer.U32(clientId);
        return std::vector<std::uint8_t>(buffer.data(), buffer.data() + writer.Size());
    }

    std::vector<std::uint8_t> Bytes(const std::string &text)
    {
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }

    /// A small deterministic generator, so a fuzz run is the same on every platform
    class XorShift
    {
    public:
        explicit XorShift(const std::uint64_t seed) : _state(seed) {}

        std::uint64_t Next()
        {
            _state ^= _state << 13;
            _state ^= _state >> 7;
            _state ^= _state << 17;
            return _state;
        }

    private:
        std::uint64_t _state;
    };
}

TEST(NetworkProtocolTest, EveryMessageRoundTrips)
{
    const auto hello = DecodeServerMessage(Encode(SampleHello()));
    ASSERT_TRUE(hello.has_value());
    const auto *helloBody = std::get_if<ServerHelloMessage>(&*hello);
    ASSERT_NE(helloBody, nullptr);
    EXPECT_EQ(helloBody->wireVersion, kWireVersion);
    EXPECT_EQ(helloBody->clientId, 42u);
    EXPECT_EQ(helloBody->fingerprint.engineVersion, "1.2.3");
    EXPECT_EQ(helloBody->fingerprint.sceneHash, 0x0123456789ABCDEFull);
    EXPECT_EQ(helloBody->fingerprint.registryHash, 0xFEDCBA9876543210ull);
    EXPECT_EQ(helloBody->fingerprint.scenePath, "res://scenes/main.scene");
    EXPECT_EQ(helloBody->fingerprint.sceneObjectCount, 7u);
    EXPECT_EQ(helloBody->fingerprint.prefabCount, 3u);

    const auto spawn = DecodeServerMessage(Encode(SampleSpawn()));
    ASSERT_TRUE(spawn.has_value());
    const auto *spawnBody = std::get_if<SpawnMessage>(&*spawn);
    ASSERT_NE(spawnBody, nullptr);
    EXPECT_EQ(spawnBody->id.value, 4096u);
    EXPECT_EQ(spawnBody->prefabIndex, 17);
    EXPECT_EQ(spawnBody->owner, 3u);

    const auto despawn = DecodeServerMessage(Encode(DespawnMessage{NetworkId{9}}));
    ASSERT_TRUE(despawn.has_value());
    const auto *despawnBody = std::get_if<DespawnMessage>(&*despawn);
    ASSERT_NE(despawnBody, nullptr);
    EXPECT_EQ(despawnBody->id.value, 9u);

    const auto ready = DecodeServerMessage(Encode(WorldReadyMessage{5, 2}));
    ASSERT_TRUE(ready.has_value());
    const auto *readyBody = std::get_if<WorldReadyMessage>(&*ready);
    ASSERT_NE(readyBody, nullptr);
    EXPECT_EQ(readyBody->spawned, 5u);
    EXPECT_EQ(readyBody->destroyedSceneObjects, 2u);

    const auto accept = DecodeClientMessage(Encode(SampleAccept()));
    ASSERT_TRUE(accept.has_value());
    const auto *acceptBody = std::get_if<ClientAcceptMessage>(&*accept);
    ASSERT_NE(acceptBody, nullptr);
    EXPECT_EQ(acceptBody->wireVersion, kWireVersion);
    EXPECT_EQ(acceptBody->fingerprint.scenePath, "res://scenes/main.scene");
    EXPECT_EQ(acceptBody->fingerprint.sceneHash, 0x0123456789ABCDEFull);

    const auto reject = DecodeClientMessage(Encode(ClientRejectMessage{JoinRefusal::Registry}));
    ASSERT_TRUE(reject.has_value());
    const auto *rejectBody = std::get_if<ClientRejectMessage>(&*reject);
    ASSERT_NE(rejectBody, nullptr);
    EXPECT_EQ(rejectBody->reason, JoinRefusal::Registry);
}

TEST(NetworkProtocolTest, MessagesAreSmallAndTheLargestFitsTheCap)
{
    EXPECT_EQ(Encode(SampleSpawn()).size(), 11u);
    EXPECT_EQ(Encode(DespawnMessage{NetworkId{1}}).size(), 5u);
    EXPECT_EQ(Encode(WorldReadyMessage{}).size(), 9u);
    EXPECT_EQ(Encode(ClientRejectMessage{JoinRefusal::ObjectCount}).size(), 2u);

    ServerHelloMessage hello;
    hello.fingerprint = LongestFingerprint();
    hello.clientId = 0xFFFFFFFFu;
    const std::vector<std::uint8_t> longest = Encode(hello);
    EXPECT_EQ(longest.size(), 318u);
    EXPECT_LE(longest.size(), kMaxMessageBytes);
    ASSERT_TRUE(DecodeServerMessage(longest).has_value());

    ClientAcceptMessage accept;
    accept.fingerprint = LongestFingerprint();
    EXPECT_LE(Encode(accept).size(), kMaxMessageBytes);
    ASSERT_TRUE(DecodeClientMessage(Encode(accept)).has_value());
}

TEST(NetworkProtocolTest, EveryTruncationIsRejected)
{
    for (const std::vector<std::uint8_t> &message : ValidServerMessages())
    {
        for (std::size_t length = 0; length < message.size(); ++length)
        {
            const std::vector<std::uint8_t> cut(message.begin(), message.begin() + static_cast<std::ptrdiff_t>(length));
            const auto decoded = DecodeServerMessage(cut);
            EXPECT_FALSE(decoded.has_value()) << "type " << static_cast<int>(message[0]) << " cut to " << length;
            if (length == 0)
            {
                ASSERT_FALSE(decoded.has_value());
                EXPECT_EQ(decoded.error(), DecodeError::Empty);
            }
        }
    }
    for (const std::vector<std::uint8_t> &message : ValidClientMessages())
    {
        for (std::size_t length = 0; length < message.size(); ++length)
        {
            const std::vector<std::uint8_t> cut(message.begin(), message.begin() + static_cast<std::ptrdiff_t>(length));
            EXPECT_FALSE(DecodeClientMessage(cut).has_value())
                << "type " << static_cast<int>(message[0]) << " cut to " << length;
        }
    }
}

TEST(NetworkProtocolTest, BytesAfterTheEndAreRejected)
{
    for (std::vector<std::uint8_t> message : ValidServerMessages())
    {
        message.push_back(0);
        const auto decoded = DecodeServerMessage(message);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error(), DecodeError::TrailingBytes);
    }
    for (std::vector<std::uint8_t> message : ValidClientMessages())
    {
        message.push_back(0);
        const auto decoded = DecodeClientMessage(message);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error(), DecodeError::TrailingBytes);
    }
}

TEST(NetworkProtocolTest, AClientCannotSayWhatOnlyTheServerSays)
{
    // The decoder the server uses on client input refuses every server message: a client can never spawn or despawn
    for (const std::vector<std::uint8_t> &message : ValidServerMessages())
    {
        const auto decoded = DecodeClientMessage(message);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error(), DecodeError::WrongDirection);
    }
    // ...and the decoder a client uses refuses a client message
    for (const std::vector<std::uint8_t> &message : ValidClientMessages())
    {
        const auto decoded = DecodeServerMessage(message);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error(), DecodeError::WrongDirection);
    }
}

TEST(NetworkProtocolTest, UnknownTypesAreRejected)
{
    for (int type = 0; type < 256; ++type)
    {
        const bool serverType = type >= 0x01 && type <= 0x04;
        const bool clientType = type == 0x81 || type == 0x82;
        const std::vector<std::uint8_t> message = {static_cast<std::uint8_t>(type), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const auto asServer = DecodeServerMessage(message);
        const auto asClient = DecodeClientMessage(message);
        if (!serverType && !clientType)
        {
            ASSERT_FALSE(asServer.has_value());
            EXPECT_EQ(asServer.error(), DecodeError::UnknownType) << "type " << type;
            ASSERT_FALSE(asClient.has_value());
            EXPECT_EQ(asClient.error(), DecodeError::UnknownType) << "type " << type;
        }
    }
    EXPECT_EQ(DecodeServerMessage({}).error(), DecodeError::Empty);
    EXPECT_EQ(DecodeClientMessage({}).error(), DecodeError::Empty);
}

TEST(NetworkProtocolTest, ADifferentWireVersionIsNamedNotParsed)
{
    std::vector<std::uint8_t> hello = Encode(SampleHello());
    hello[1] = 77;
    hello[2] = 0;
    const auto decoded = DecodeServerMessage(hello);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error(), DecodeError::UnsupportedVersion);
    ASSERT_TRUE(PeekWireVersion(hello).has_value());
    EXPECT_EQ(*PeekWireVersion(hello), 77);

    // Even a hello whose body is garbage past the version says its version
    const std::vector<std::uint8_t> bare = {static_cast<std::uint8_t>(MessageType::ServerHello), 9, 0};
    EXPECT_EQ(*PeekWireVersion(bare), 9);
    EXPECT_EQ(DecodeServerMessage(bare).error(), DecodeError::UnsupportedVersion);

    std::vector<std::uint8_t> accept = Encode(SampleAccept());
    accept[1] = 7;
    EXPECT_EQ(DecodeClientMessage(accept).error(), DecodeError::UnsupportedVersion);

    EXPECT_FALSE(PeekWireVersion(Encode(SampleSpawn())).has_value());
    EXPECT_FALSE(PeekWireVersion({}).has_value());
    EXPECT_FALSE(PeekWireVersion(std::vector<std::uint8_t>{static_cast<std::uint8_t>(MessageType::ServerHello), 1})
                     .has_value());
}

TEST(NetworkProtocolTest, TextsAreCappedAndMustBePrintable)
{
    const std::vector<std::uint8_t> engine = Bytes("1.0.0");
    const std::vector<std::uint8_t> path = Bytes("res://a.scene");
    const auto good = HandmadeHello(kWireVersion, 5, engine, 13, path, 1, 1, 1);
    ASSERT_TRUE(DecodeServerMessage(good).has_value());

    // An engine version over its cap of 32 bytes (the length byte alone says so: nothing is taken or allocated)
    const std::vector<std::uint8_t> longEngine(33, 'v');
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 33, longEngine, 13, path, 1, 1, 1)).error(),
              DecodeError::BadText);
    // A length that claims more than the message holds
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 200, Bytes("short"), 1, 1, 1)).error(),
              DecodeError::Truncated);
    // A control character, a byte over 0x7E, and a newline that would split a log line
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, Bytes("1.0\x07.0"), 13, path, 1, 1, 1)).error(),
              DecodeError::BadText);
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, Bytes("1.0\xC3.0"), 13, path, 1, 1, 1)).error(),
              DecodeError::BadText);
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 13, Bytes("res://a\n.scen"), 1, 1, 1)).error(),
              DecodeError::BadText);
}

TEST(NetworkProtocolTest, ValuesOutOfRangeAreRejected)
{
    const std::vector<std::uint8_t> engine = Bytes("1.0.0");
    const std::vector<std::uint8_t> path = Bytes("res://a.scene");

    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 13, path, kMaxNetworkObjects + 1, 1, 1)).error(),
              DecodeError::BadValue);
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 13, path, 0xFFFFFFFFu, 1, 1)).error(),
              DecodeError::BadValue);
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 13, path, 1, kMaxPrefabs + 1, 1)).error(),
              DecodeError::BadValue);
    // A client id of 0 is the server's, never a client's
    EXPECT_EQ(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 13, path, 1, 1, 0)).error(),
              DecodeError::BadValue);
    ASSERT_TRUE(DecodeServerMessage(HandmadeHello(kWireVersion, 5, engine, 13, path, kMaxNetworkObjects, kMaxPrefabs, 1))
                    .has_value());

    EXPECT_EQ(DecodeServerMessage(Encode(SpawnMessage{NetworkId{0}, 1, 0})).error(), DecodeError::BadValue);
    EXPECT_EQ(DecodeServerMessage(Encode(SpawnMessage{NetworkId{5}, kMaxPrefabs, 0})).error(), DecodeError::BadValue);
    EXPECT_TRUE(DecodeServerMessage(Encode(SpawnMessage{NetworkId{5}, kMaxPrefabs - 1, 0})).has_value());
    EXPECT_EQ(DecodeServerMessage(Encode(DespawnMessage{NetworkId{0}})).error(), DecodeError::BadValue);
    EXPECT_EQ(DecodeServerMessage(Encode(WorldReadyMessage{kMaxNetworkObjects + 1, 0})).error(), DecodeError::BadValue);
    EXPECT_EQ(DecodeServerMessage(Encode(WorldReadyMessage{0, kMaxNetworkObjects + 1})).error(), DecodeError::BadValue);
    EXPECT_TRUE(DecodeServerMessage(Encode(WorldReadyMessage{kMaxNetworkObjects, kMaxNetworkObjects})).has_value());

    EXPECT_EQ(DecodeClientMessage(Encode(ClientRejectMessage{JoinRefusal::None})).error(), DecodeError::BadValue);
    const std::vector<std::uint8_t> rejectSix = {static_cast<std::uint8_t>(MessageType::ClientReject), 6};
    const std::vector<std::uint8_t> reject255 = {static_cast<std::uint8_t>(MessageType::ClientReject), 255};
    EXPECT_EQ(DecodeClientMessage(rejectSix).error(), DecodeError::BadValue);
    EXPECT_EQ(DecodeClientMessage(reject255).error(), DecodeError::BadValue);
}

TEST(NetworkProtocolTest, EncodingMakesTextSafeAndCutsItToItsCap)
{
    ServerHelloMessage hello = SampleHello();
    hello.fingerprint.engineVersion = "a\x01" "b\n" "c";
    hello.fingerprint.scenePath = std::string(400, 'x');
    const auto decoded = DecodeServerMessage(Encode(hello));
    ASSERT_TRUE(decoded.has_value());
    const auto *body = std::get_if<ServerHelloMessage>(&*decoded);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->fingerprint.engineVersion, "a?b?c");
    EXPECT_EQ(body->fingerprint.scenePath, std::string(kMaxScenePathLength, 'x'));

    hello.fingerprint.engineVersion = std::string(100, 'e');
    const auto cut = DecodeServerMessage(Encode(hello));
    ASSERT_TRUE(cut.has_value());
    EXPECT_EQ(std::get<ServerHelloMessage>(*cut).fingerprint.engineVersion.size(), kMaxEngineVersionLength);
}

TEST(NetworkProtocolTest, NoMutationOfAValidMessageCrashesTheDecoderAndWhatDecodesIsCanonical)
{
    for (const std::vector<std::uint8_t> &original : ValidServerMessages())
    {
        for (std::size_t position = 0; position < original.size(); ++position)
        {
            for (int value = 0; value < 256; ++value)
            {
                std::vector<std::uint8_t> mutated = original;
                mutated[position] = static_cast<std::uint8_t>(value);
                const auto decoded = DecodeServerMessage(mutated);
                if (decoded.has_value())
                {
                    ASSERT_EQ(Reencode(*decoded), mutated) << "position " << position << " value " << value;
                }
            }
        }
    }
    for (const std::vector<std::uint8_t> &original : ValidClientMessages())
    {
        for (std::size_t position = 0; position < original.size(); ++position)
        {
            for (int value = 0; value < 256; ++value)
            {
                std::vector<std::uint8_t> mutated = original;
                mutated[position] = static_cast<std::uint8_t>(value);
                const auto decoded = DecodeClientMessage(mutated);
                if (decoded.has_value())
                {
                    ASSERT_EQ(Reencode(*decoded), mutated) << "position " << position << " value " << value;
                }
            }
        }
    }
}

TEST(NetworkProtocolTest, RandomBytesNeverCrashEitherDecoder)
{
    XorShift random(0x4E32'4E42'0001ull);
    for (int round = 0; round < 20000; ++round)
    {
        const std::size_t length = static_cast<std::size_t>(random.Next() % 700);
        std::vector<std::uint8_t> bytes(length);
        for (std::uint8_t &byte : bytes)
        {
            byte = static_cast<std::uint8_t>(random.Next() & 0xFFu);
        }
        // Half of the rounds start with a real type byte, so the field decoders get exercised
        if (!bytes.empty() && (round % 2) == 0)
        {
            static constexpr std::array<std::uint8_t, 6> kTypes = {0x01, 0x02, 0x03, 0x04, 0x81, 0x82};
            bytes[0] = kTypes[static_cast<std::size_t>(random.Next() % kTypes.size())];
        }
        const auto asServer = DecodeServerMessage(bytes);
        if (asServer.has_value())
        {
            ASSERT_EQ(Reencode(*asServer), bytes);
        }
        const auto asClient = DecodeClientMessage(bytes);
        if (asClient.has_value())
        {
            ASSERT_EQ(Reencode(*asClient), bytes);
        }
    }
}

TEST(NetworkProtocolTest, DecodeErrorsHaveNames)
{
    for (const DecodeError error :
         {DecodeError::Empty, DecodeError::UnknownType, DecodeError::WrongDirection, DecodeError::UnsupportedVersion,
          DecodeError::Truncated, DecodeError::TrailingBytes, DecodeError::BadText, DecodeError::BadValue})
    {
        EXPECT_FALSE(ToString(error).empty());
        EXPECT_NE(ToString(error), "unknown error");
    }
}
