#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkId.hpp"

namespace N2Engine::Network
{
    // The world messages (netcode stage B1), carried one per message on the reliable-ordered channel of an n2net
    // Connection. Every integer is little-endian. A message is a type byte and a body; the type byte also says who may
    // send it, so a message in the wrong direction is rejected whatever it contains:
    //
    //   server to client (0x01 to 0x7F)
    //     0x01 ServerHello   u16 wire version, fingerprint, u32 client id (not 0)
    //     0x02 Spawn         u32 net id (not 0), u16 prefab index, u32 owner client id (0: the server)
    //     0x03 Despawn       u32 net id (not 0)
    //     0x04 WorldReady    u32 spawned objects sent in the join state, u32 destroyed scene objects sent
    //   client to server (0x81 to 0xFF)
    //     0x81 ClientAccept  u16 wire version, fingerprint
    //     0x82 ClientReject  u8 reason (1 to 5, JoinRefusal)
    //
    //   fingerprint: u8 length + engine version text (at most 32 bytes), u64 scene hash, u64 registry hash,
    //                u8 length + scene path text (at most 255 bytes), u32 scene object count, u16 prefab count
    //   text: printable ASCII only (0x20 to 0x7E), so it can go into a log line as it is
    //
    // The first field of both hellos is the wire version, so a peer that does not know the version can say so without
    // reading what follows. A decoder reads every length against the bytes that remain and against its cap before
    // taking anything, allocates only the two bounded texts of a fingerprint (never anything a count asks for: the
    // counts are plain integers, checked against kMaxNetworkObjects and kMaxPrefabs), refuses a message with bytes
    // left over, and never trusts a count to be true. No message is larger than kMaxMessageBytes.

    /// The version of the world messages above. A change to any message layout bumps it.
    inline constexpr std::uint16_t kWireVersion = 1;

    /// The largest world message either side sends or accepts (a ServerHello with the longest fingerprint is the
    /// largest: 1 type + 2 version + 311 fingerprint + 4 client id = 318 bytes). The Connection's reliable message size
    /// is set to it, so a peer that sends more fails its own connection.
    inline constexpr std::size_t kMaxMessageBytes = 512;

    enum class MessageType : std::uint8_t
    {
        ServerHello = 0x01,
        Spawn = 0x02,
        Despawn = 0x03,
        WorldReady = 0x04,
        ClientAccept = 0x81,
        ClientReject = 0x82,
    };

    struct ServerHelloMessage
    {
        std::uint16_t wireVersion = kWireVersion;
        Fingerprint fingerprint;
        ClientId clientId = 0;
    };

    struct SpawnMessage
    {
        NetworkId id;
        std::uint16_t prefabIndex = 0;
        ClientId owner = kServerClientId;
    };

    struct DespawnMessage
    {
        NetworkId id;
    };

    struct WorldReadyMessage
    {
        std::uint32_t spawned = 0;
        std::uint32_t destroyedSceneObjects = 0;
    };

    struct ClientAcceptMessage
    {
        std::uint16_t wireVersion = kWireVersion;
        Fingerprint fingerprint;
    };

    struct ClientRejectMessage
    {
        JoinRefusal reason = JoinRefusal::None;
    };

    using ServerMessage = std::variant<ServerHelloMessage, SpawnMessage, DespawnMessage, WorldReadyMessage>;
    using ClientMessage = std::variant<ClientAcceptMessage, ClientRejectMessage>;

    enum class DecodeError : std::uint8_t
    {
        /// Zero bytes
        Empty,
        /// A type byte no message has
        UnknownType,
        /// A message of the other direction (a client sending Spawn is this)
        WrongDirection,
        /// A hello or accept whose wire version is not kWireVersion
        UnsupportedVersion,
        /// The body ends before the message does
        Truncated,
        /// Bytes after the end of the message
        TrailingBytes,
        /// A text over its cap or holding a byte outside printable ASCII
        BadText,
        /// A value outside its range (an id or client id of 0, a count over its cap, an unknown refusal reason)
        BadValue,
    };

    [[nodiscard]] std::string_view ToString(DecodeError error) noexcept;

    /// Decodes one message a client received. Never throws on malformed input (it returns the reason), reads only the
    /// bytes it is given and allocates nothing but the two bounded texts of a fingerprint.
    [[nodiscard]] std::expected<ServerMessage, DecodeError> DecodeServerMessage(std::span<const std::uint8_t> bytes);
    /// Decodes one message the server received from a client
    [[nodiscard]] std::expected<ClientMessage, DecodeError> DecodeClientMessage(std::span<const std::uint8_t> bytes);

    /// The wire version of a ServerHello or ClientAccept (the two bytes after the type), for an error message after
    /// DecodeError::UnsupportedVersion; nullopt when `bytes` is neither or too short
    [[nodiscard]] std::optional<std::uint16_t> PeekWireVersion(std::span<const std::uint8_t> bytes);

    /// The encoded message. A text longer than its cap is cut, a byte outside printable ASCII becomes '?'.
    [[nodiscard]] std::vector<std::uint8_t> Encode(const ServerHelloMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const SpawnMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const DespawnMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const WorldReadyMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const ClientAcceptMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const ClientRejectMessage &message);
}
