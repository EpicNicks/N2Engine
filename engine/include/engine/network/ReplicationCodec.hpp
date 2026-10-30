#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/NetworkHash.hpp"
#include "engine/network/n2net/ByteIO.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::Network
{
    // The wire codecs of replicated fields (netcode stage B2). A field travels as the bytes below, picked by its
    // FieldKind (and FieldInfo::range / replicationStep); both ends compute the same layout from the same field list,
    // so nothing on the wire says what kind a value is. Every integer is little-endian, a varint is LEB128 (7 bits per
    // byte, low bits first, canonical: no trailing zero byte, at most 10 bytes) and a zig-zag varint maps a signed
    // number n to (n << 1) ^ (n >> 63) first.
    //
    //   Bool           1 byte, 0 or 1
    //   Int            zig-zag varint of the 64-bit value
    //   Float          replicationStep > 0: zig-zag varint of round(value / step), the whole-step count clamped to 32
    //                  bits (the sender logs a warning once per field when it clamps); else with a finite Range: u16 = round((value - low) / (high - low) * 65535), value clamped
    //                  to the range; else the IEEE-754 float32 bits (u32)
    //   Vector2/3/4    each axis (x, y, z; Vector4: w, x, y, z) as a Float with a step, or float32 bits
    //   Quaternion     smallest-three: u32 = largest component index (2 bits) then the other three, each 10 bits across
    //                  [-1/sqrt 2, 1/sqrt 2], the largest taken positive (the sign of q does not matter). The error is
    //                  at most half a step, sqrt 2 / 1023 / 2 = 0.00069, per sent component. The largest is rebuilt as
    //                  sqrt(1 - the sum of the other three squared) and nothing is renormalised, so it carries their
    //                  errors: at worst about 3 x 0.00069 = 0.0021 (all four components 0.5, every error the same way:
    //                  sqrt(0.25 - 3 x 0.00069 - ...) = 0.4979). The rotation is then off by about 2 x 0.00069 x sqrt 12 =
    //                  0.0048 radians (0.27 degrees), under 0.005 radians (0.3 degrees).
    //   Color          8 bits per channel clamped to [0, 1]: r, g, b, a (Common::Color), x, y, z (a Vector3 marked
    //                  AsColor) or w, x, y, z (a Vector4 marked AsColor); the error is at most 1/510
    //   String         varint length (at most kMaxReplicatedStringBytes) and the UTF-8 bytes; a longer string is cut at
    //                  a character boundary, invalid UTF-8 is refused
    //   Enum           varint index into FieldInfo::enumOptions
    //   AssetRef       u8 0 (none), or u8 1 and the 16 UUID bytes
    //   GameObjectRef  varint net id of the object; 0 is none (also what an object without a NetworkObject becomes)
    //
    // A decoder reads every length against the bytes that remain and its cap before taking anything and allocates only
    // what a cap bounds (a string of at most kMaxReplicatedStringBytes). Not replicable: the lists, ComponentRef, Json.

    /// The longest string a replicated field carries, in bytes
    inline constexpr std::size_t kMaxReplicatedStringBytes = 128;
    /// The most replicated fields one object has (the field mask is at most this many bits); fields beyond are ignored
    inline constexpr std::size_t kMaxReplicatedFieldsPerObject = 64;
    /// The most bytes of one object's replicated fields at their longest (mask included); an object whose fields could
    /// exceed it replicates nothing (an error is logged)
    inline constexpr std::size_t kMaxReplicatedEntryBytes = 900;

    /// Appends to a byte vector
    class WireWriter
    {
    public:
        explicit WireWriter(std::vector<std::uint8_t> &out) noexcept : _out(out) {}

        void U8(const std::uint8_t value) { _out.push_back(value); }

        void U16(const std::uint16_t value)
        {
            _out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            _out.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        void U32(const std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                _out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }

        void Varint(std::uint64_t value)
        {
            while (value >= 0x80)
            {
                _out.push_back(static_cast<std::uint8_t>((value & 0x7Fu) | 0x80u));
                value >>= 7;
            }
            _out.push_back(static_cast<std::uint8_t>(value));
        }

        void ZigZag(const std::int64_t value)
        {
            Varint((static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63));
        }

        void Bytes(const std::span<const std::uint8_t> bytes) { _out.insert(_out.end(), bytes.begin(), bytes.end()); }

        [[nodiscard]] std::size_t Size() const noexcept { return _out.size(); }

    private:
        std::vector<std::uint8_t> &_out;
    };

    /// Reads a canonical varint (at most 10 bytes); false if the bytes end, it is longer than 10 bytes, it overflows 64
    /// bits or it has a trailing zero byte
    [[nodiscard]] bool ReadVarint(N2Net::ByteReader &reader, std::uint64_t &value) noexcept;
    [[nodiscard]] bool ReadZigZag(N2Net::ByteReader &reader, std::int64_t &value) noexcept;

    /// How a GameObject reference crosses the wire: by the net id of its object
    class IReplicationReferences
    {
    public:
        virtual ~IReplicationReferences() = default;
        /// The net id of the GameObject with this UUID text; 0 when it has none (not networked, or unknown)
        [[nodiscard]] virtual std::uint32_t NetIdOf(const std::string &uuid) const = 0;
        /// The UUID text of the GameObject with this net id; nullopt when this side has no such object (yet)
        [[nodiscard]] virtual std::optional<std::string> UuidOf(std::uint32_t netId) const = 0;
    };

    enum class DecodeStatus : std::uint8_t
    {
        Ok,
        /// The bytes are not a value of the kind (truncated, over a cap, out of range, invalid UTF-8)
        Malformed,
        /// A GameObject reference to a net id this side has no object for: well-formed, but not applicable now
        UnknownReference,
    };

    /// The most bytes EncodeField writes for the field (0 for a kind that cannot be replicated)
    [[nodiscard]] std::size_t MaxEncodedBytes(const FieldInfo &field) noexcept;

    /// Appends the wire bytes of `value` (the JSON the component writes for the field). False, with `out` as it was,
    /// when the field is not replicable or the value is not one (a wrong JSON type, a number a float cannot hold, a
    /// name that is not an enum option, invalid UTF-8). `references` may be null: a GameObjectRef then encodes as none.
    [[nodiscard]] bool EncodeField(const FieldInfo &field, const nlohmann::json &value,
                                   const IReplicationReferences *references, std::vector<std::uint8_t> &out);

    /// Reads one field of the kind from `reader` into `value`, checked with ValidateFieldValue and clamped to the
    /// field's range. Never throws, never reads past the reader's bytes.
    [[nodiscard]] DecodeStatus DecodeField(const FieldInfo &field, N2Net::ByteReader &reader,
                                           const IReplicationReferences *references, nlohmann::json &value);

    /// Feeds everything about the field that decides its bytes on the wire (name, container, kind, shape, step, range,
    /// enum options): two peers whose fields hash alike read each other's bytes
    void HashFieldFormat(Fnv1a64 &hash, const FieldInfo &field);
}
