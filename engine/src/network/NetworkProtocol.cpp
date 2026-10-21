#include "engine/network/NetworkProtocol.hpp"

#include <array>
#include <string>
#include <utility>

#include "engine/network/n2net/ByteIO.hpp"

namespace N2Engine::Network
{
    namespace
    {
        using N2Net::ByteReader;
        using N2Net::ByteWriter;

        constexpr auto kTypeServerHello = static_cast<std::uint8_t>(MessageType::ServerHello);
        constexpr auto kTypeSpawn = static_cast<std::uint8_t>(MessageType::Spawn);
        constexpr auto kTypeDespawn = static_cast<std::uint8_t>(MessageType::Despawn);
        constexpr auto kTypeWorldReady = static_cast<std::uint8_t>(MessageType::WorldReady);
        constexpr auto kTypeClientAccept = static_cast<std::uint8_t>(MessageType::ClientAccept);
        constexpr auto kTypeClientReject = static_cast<std::uint8_t>(MessageType::ClientReject);

        constexpr std::uint8_t kLastRefusal = static_cast<std::uint8_t>(JoinRefusal::ObjectCount);

        void WriteU64(ByteWriter &writer, const std::uint64_t value)
        {
            writer.U32(static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
            writer.U32(static_cast<std::uint32_t>(value >> 32));
        }

        std::uint64_t ReadU64(ByteReader &reader)
        {
            // Two statements: the order of two reads in one expression is unspecified
            const std::uint64_t low = reader.U32();
            const std::uint64_t high = reader.U32();
            return low | (high << 32);
        }

        void WriteText(ByteWriter &writer, const std::string_view text, const std::size_t maxLength)
        {
            const std::string clean = SanitizeText(text, maxLength);
            writer.U8(static_cast<std::uint8_t>(clean.size()));
            writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(clean.data()), clean.size()));
        }

        /// nullopt on success. The length byte is checked against the cap and against the bytes that remain before
        /// anything is taken or allocated.
        std::optional<DecodeError> ReadText(ByteReader &reader, const std::size_t maxLength, std::string &out)
        {
            const std::size_t length = reader.U8();
            if (reader.Failed())
            {
                return DecodeError::Truncated;
            }
            if (length > maxLength)
            {
                return DecodeError::BadText;
            }
            const std::span<const std::uint8_t> bytes = reader.Bytes(length);
            if (reader.Failed())
            {
                return DecodeError::Truncated;
            }
            for (const std::uint8_t byte : bytes)
            {
                if (byte < 0x20 || byte > 0x7E)
                {
                    return DecodeError::BadText;
                }
            }
            if (bytes.empty())
            {
                out.clear();
            }
            else
            {
                out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            }
            return std::nullopt;
        }

        void WriteFingerprint(ByteWriter &writer, const Fingerprint &fingerprint)
        {
            WriteText(writer, fingerprint.engineVersion, kMaxEngineVersionLength);
            WriteU64(writer, fingerprint.sceneHash);
            WriteU64(writer, fingerprint.registryHash);
            WriteText(writer, fingerprint.scenePath, kMaxScenePathLength);
            writer.U32(fingerprint.sceneObjectCount);
            writer.U16(fingerprint.prefabCount);
        }

        std::optional<DecodeError> ReadFingerprint(ByteReader &reader, Fingerprint &fingerprint)
        {
            if (const auto error = ReadText(reader, kMaxEngineVersionLength, fingerprint.engineVersion))
            {
                return error;
            }
            fingerprint.sceneHash = ReadU64(reader);
            fingerprint.registryHash = ReadU64(reader);
            if (const auto error = ReadText(reader, kMaxScenePathLength, fingerprint.scenePath))
            {
                return error;
            }
            fingerprint.sceneObjectCount = reader.U32();
            fingerprint.prefabCount = reader.U16();
            if (reader.Failed())
            {
                return DecodeError::Truncated;
            }
            if (fingerprint.sceneObjectCount > kMaxNetworkObjects || fingerprint.prefabCount > kMaxPrefabs)
            {
                return DecodeError::BadValue;
            }
            return std::nullopt;
        }

        /// What is left to check once a message's fields are read
        std::optional<DecodeError> CheckEnd(const ByteReader &reader)
        {
            if (reader.Failed())
            {
                return DecodeError::Truncated;
            }
            if (reader.Remaining() != 0)
            {
                return DecodeError::TrailingBytes;
            }
            return std::nullopt;
        }

        std::vector<std::uint8_t> Finish(const std::array<std::uint8_t, kMaxMessageBytes> &buffer, const ByteWriter &writer)
        {
            // Cannot overflow: the largest message is 318 bytes
            return std::vector<std::uint8_t>(buffer.data(), buffer.data() + writer.Size());
        }
    }

    std::string_view ToString(const DecodeError error) noexcept
    {
        switch (error)
        {
        case DecodeError::Empty:
            return "empty message";
        case DecodeError::UnknownType:
            return "unknown message type";
        case DecodeError::WrongDirection:
            return "message of the other direction";
        case DecodeError::UnsupportedVersion:
            return "unsupported wire version";
        case DecodeError::Truncated:
            return "truncated message";
        case DecodeError::TrailingBytes:
            return "bytes after the end of the message";
        case DecodeError::BadText:
            return "text too long or not printable ASCII";
        case DecodeError::BadValue:
            return "value out of range";
        }
        return "unknown error";
    }

    std::expected<ServerMessage, DecodeError> DecodeServerMessage(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        ByteReader reader(bytes);
        const std::uint8_t type = reader.U8();
        switch (type)
        {
        case kTypeServerHello:
        {
            ServerHelloMessage message;
            message.wireVersion = reader.U16();
            if (reader.Failed())
            {
                return std::unexpected(DecodeError::Truncated);
            }
            if (message.wireVersion != kWireVersion)
            {
                return std::unexpected(DecodeError::UnsupportedVersion);
            }
            if (const auto error = ReadFingerprint(reader, message.fingerprint))
            {
                return std::unexpected(*error);
            }
            message.clientId = reader.U32();
            if (const auto error = CheckEnd(reader))
            {
                return std::unexpected(*error);
            }
            if (message.clientId == kServerClientId)
            {
                return std::unexpected(DecodeError::BadValue);
            }
            return ServerMessage(std::move(message));
        }
        case kTypeSpawn:
        {
            SpawnMessage message;
            message.id.value = reader.U32();
            message.prefabIndex = reader.U16();
            message.owner = reader.U32();
            if (const auto error = CheckEnd(reader))
            {
                return std::unexpected(*error);
            }
            if (!message.id.IsValid() || message.prefabIndex >= kMaxPrefabs)
            {
                return std::unexpected(DecodeError::BadValue);
            }
            return ServerMessage(message);
        }
        case kTypeDespawn:
        {
            DespawnMessage message;
            message.id.value = reader.U32();
            if (const auto error = CheckEnd(reader))
            {
                return std::unexpected(*error);
            }
            if (!message.id.IsValid())
            {
                return std::unexpected(DecodeError::BadValue);
            }
            return ServerMessage(message);
        }
        case kTypeWorldReady:
        {
            WorldReadyMessage message;
            message.spawned = reader.U32();
            message.destroyedSceneObjects = reader.U32();
            if (const auto error = CheckEnd(reader))
            {
                return std::unexpected(*error);
            }
            if (message.spawned > kMaxNetworkObjects || message.destroyedSceneObjects > kMaxNetworkObjects)
            {
                return std::unexpected(DecodeError::BadValue);
            }
            return ServerMessage(message);
        }
        case kTypeClientAccept:
        case kTypeClientReject:
            return std::unexpected(DecodeError::WrongDirection);
        default:
            return std::unexpected(DecodeError::UnknownType);
        }
    }

    std::expected<ClientMessage, DecodeError> DecodeClientMessage(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        ByteReader reader(bytes);
        const std::uint8_t type = reader.U8();
        switch (type)
        {
        case kTypeClientAccept:
        {
            ClientAcceptMessage message;
            message.wireVersion = reader.U16();
            if (reader.Failed())
            {
                return std::unexpected(DecodeError::Truncated);
            }
            if (message.wireVersion != kWireVersion)
            {
                return std::unexpected(DecodeError::UnsupportedVersion);
            }
            if (const auto error = ReadFingerprint(reader, message.fingerprint))
            {
                return std::unexpected(*error);
            }
            if (const auto error = CheckEnd(reader))
            {
                return std::unexpected(*error);
            }
            return ClientMessage(std::move(message));
        }
        case kTypeClientReject:
        {
            const std::uint8_t reason = reader.U8();
            if (const auto error = CheckEnd(reader))
            {
                return std::unexpected(*error);
            }
            if (reason == 0 || reason > kLastRefusal)
            {
                return std::unexpected(DecodeError::BadValue);
            }
            ClientRejectMessage message;
            message.reason = static_cast<JoinRefusal>(reason);
            return ClientMessage(message);
        }
        case kTypeServerHello:
        case kTypeSpawn:
        case kTypeDespawn:
        case kTypeWorldReady:
            return std::unexpected(DecodeError::WrongDirection);
        default:
            return std::unexpected(DecodeError::UnknownType);
        }
    }

    std::optional<std::uint16_t> PeekWireVersion(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() < 3 || (bytes[0] != kTypeServerHello && bytes[0] != kTypeClientAccept))
        {
            return std::nullopt;
        }
        ByteReader reader(bytes);
        (void)reader.U8();
        const std::uint16_t version = reader.U16();
        return reader.Failed() ? std::nullopt : std::optional<std::uint16_t>(version);
    }

    std::vector<std::uint8_t> Encode(const ServerHelloMessage &message)
    {
        std::array<std::uint8_t, kMaxMessageBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kTypeServerHello);
        writer.U16(message.wireVersion);
        WriteFingerprint(writer, message.fingerprint);
        writer.U32(message.clientId);
        return Finish(buffer, writer);
    }

    std::vector<std::uint8_t> Encode(const SpawnMessage &message)
    {
        std::array<std::uint8_t, kMaxMessageBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kTypeSpawn);
        writer.U32(message.id.value);
        writer.U16(message.prefabIndex);
        writer.U32(message.owner);
        return Finish(buffer, writer);
    }

    std::vector<std::uint8_t> Encode(const DespawnMessage &message)
    {
        std::array<std::uint8_t, kMaxMessageBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kTypeDespawn);
        writer.U32(message.id.value);
        return Finish(buffer, writer);
    }

    std::vector<std::uint8_t> Encode(const WorldReadyMessage &message)
    {
        std::array<std::uint8_t, kMaxMessageBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kTypeWorldReady);
        writer.U32(message.spawned);
        writer.U32(message.destroyedSceneObjects);
        return Finish(buffer, writer);
    }

    std::vector<std::uint8_t> Encode(const ClientAcceptMessage &message)
    {
        std::array<std::uint8_t, kMaxMessageBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kTypeClientAccept);
        writer.U16(message.wireVersion);
        WriteFingerprint(writer, message.fingerprint);
        return Finish(buffer, writer);
    }

    std::vector<std::uint8_t> Encode(const ClientRejectMessage &message)
    {
        std::array<std::uint8_t, kMaxMessageBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kTypeClientReject);
        writer.U8(static_cast<std::uint8_t>(message.reason));
        return Finish(buffer, writer);
    }
}
