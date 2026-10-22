#include "engine/network/ReplicationCodec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

#include <math/UUID.hpp>

namespace N2Engine::Network
{
    namespace
    {
        using json = nlohmann::json;

        constexpr double kSqrtHalf = 0.70710678118654752440;
        constexpr std::int64_t kMaxStepCount = 2147483647;

        bool UsesRange(const FieldInfo &field) noexcept
        {
            return field.kind == FieldKind::Float && field.range.has_value() && std::isfinite(field.range->first) &&
                   std::isfinite(field.range->second) && field.range->second > field.range->first;
        }

        std::size_t AxisBytes(const FieldInfo &field) noexcept
        {
            return field.replicationStep > 0.0 ? 5 : 4;
        }

        /// A finite number a float can hold
        bool GetNumber(const json &object, const char *key, double &out)
        {
            const auto found = object.find(key);
            if (found == object.end() || !found->is_number())
            {
                return false;
            }
            const double number = found->get<double>();
            if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                return false;
            }
            out = number;
            return true;
        }

        void PutAxis(const FieldInfo &field, const double value, WireWriter &writer)
        {
            if (field.replicationStep > 0.0)
            {
                double steps = std::round(value / field.replicationStep);
                steps = std::clamp(steps, -static_cast<double>(kMaxStepCount), static_cast<double>(kMaxStepCount));
                writer.ZigZag(static_cast<std::int64_t>(steps));
            }
            else
            {
                writer.U32(std::bit_cast<std::uint32_t>(static_cast<float>(value)));
            }
        }

        bool GetAxis(const FieldInfo &field, N2Net::ByteReader &reader, double &value)
        {
            if (field.replicationStep > 0.0)
            {
                std::int64_t steps = 0;
                if (!ReadZigZag(reader, steps))
                {
                    return false;
                }
                if (steps > kMaxStepCount || steps < -kMaxStepCount)
                {
                    return false;
                }
                value = static_cast<double>(steps) * field.replicationStep;
            }
            else
            {
                const std::uint32_t bits = reader.U32();
                if (reader.Failed())
                {
                    return false;
                }
                const float number = std::bit_cast<float>(bits);
                if (!std::isfinite(number))
                {
                    return false;
                }
                value = static_cast<double>(number);
            }
            return std::isfinite(value) && std::fabs(value) <= static_cast<double>(std::numeric_limits<float>::max());
        }

        /// True for text that is well-formed UTF-8 (no overlong form, no surrogate, nothing above U+10FFFF)
        bool IsValidUtf8(const std::string &text) noexcept
        {
            std::size_t i = 0;
            const std::size_t size = text.size();
            while (i < size)
            {
                const auto lead = static_cast<unsigned char>(text[i]);
                std::size_t extra = 0;
                std::uint32_t codePoint = 0;
                std::uint32_t minimum = 0;
                if (lead < 0x80)
                {
                    ++i;
                    continue;
                }
                if ((lead & 0xE0) == 0xC0)
                {
                    extra = 1;
                    codePoint = lead & 0x1Fu;
                    minimum = 0x80;
                }
                else if ((lead & 0xF0) == 0xE0)
                {
                    extra = 2;
                    codePoint = lead & 0x0Fu;
                    minimum = 0x800;
                }
                else if ((lead & 0xF8) == 0xF0)
                {
                    extra = 3;
                    codePoint = lead & 0x07u;
                    minimum = 0x10000;
                }
                else
                {
                    return false;
                }
                if (i + extra >= size)
                {
                    return false;
                }
                for (std::size_t k = 1; k <= extra; ++k)
                {
                    const auto next = static_cast<unsigned char>(text[i + k]);
                    if ((next & 0xC0) != 0x80)
                    {
                        return false;
                    }
                    codePoint = (codePoint << 6) | (next & 0x3Fu);
                }
                if (codePoint < minimum || codePoint > 0x10FFFFu || (codePoint >= 0xD800u && codePoint <= 0xDFFFu))
                {
                    return false;
                }
                i += extra + 1;
            }
            return true;
        }

        std::uint8_t ToByte(const double channel) noexcept
        {
            const double clamped = std::clamp(channel, 0.0, 1.0);
            return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
        }

        /// The keys of a Color-kind value, in wire order: r,g,b,a (Common::Color), x,y,z (Vector3) or w,x,y,z (Vector4)
        std::vector<const char *> ColorKeys(const FieldInfo &field)
        {
            if (field.typeName == "Vector3")
            {
                return {"x", "y", "z"};
            }
            if (field.typeName == "Vector4")
            {
                return {"w", "x", "y", "z"};
            }
            return {"r", "g", "b", "a"};
        }

        bool EncodeQuaternion(const json &value, WireWriter &writer)
        {
            if (!value.is_object())
            {
                return false;
            }
            std::array<double, 4> c{};
            if (!GetNumber(value, "w", c[0]) || !GetNumber(value, "x", c[1]) || !GetNumber(value, "y", c[2]) ||
                !GetNumber(value, "z", c[3]))
            {
                return false;
            }
            const double lengthSquared = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3];
            if (lengthSquared < 1e-12)
            {
                c = {1.0, 0.0, 0.0, 0.0};
            }
            else
            {
                const double length = std::sqrt(lengthSquared);
                for (double &component : c)
                {
                    component /= length;
                }
            }
            std::size_t largest = 0;
            for (std::size_t i = 1; i < 4; ++i)
            {
                if (std::fabs(c[i]) > std::fabs(c[largest]))
                {
                    largest = i;
                }
            }
            const double sign = c[largest] < 0.0 ? -1.0 : 1.0;
            std::uint32_t packed = static_cast<std::uint32_t>(largest);
            int shift = 2;
            for (std::size_t i = 0; i < 4; ++i)
            {
                if (i == largest)
                {
                    continue;
                }
                const double v = std::clamp(c[i] * sign, -kSqrtHalf, kSqrtHalf);
                const long quantised = std::clamp(std::lround((v + kSqrtHalf) / (2.0 * kSqrtHalf) * 1023.0), 0L, 1023L);
                packed |= static_cast<std::uint32_t>(quantised) << shift;
                shift += 10;
            }
            writer.U32(packed);
            return true;
        }

        bool DecodeQuaternion(N2Net::ByteReader &reader, json &value)
        {
            const std::uint32_t packed = reader.U32();
            if (reader.Failed())
            {
                return false;
            }
            const std::size_t largest = packed & 3u;
            std::array<double, 4> c{};
            double sumSquares = 0.0;
            int shift = 2;
            for (std::size_t i = 0; i < 4; ++i)
            {
                if (i == largest)
                {
                    continue;
                }
                const double quantised = static_cast<double>((packed >> shift) & 0x3FFu);
                c[i] = quantised / 1023.0 * (2.0 * kSqrtHalf) - kSqrtHalf;
                sumSquares += c[i] * c[i];
                shift += 10;
            }
            c[largest] = std::sqrt(std::max(0.0, 1.0 - sumSquares));
            value = json{{"w", c[0]}, {"x", c[1]}, {"y", c[2]}, {"z", c[3]}};
            return true;
        }

        std::string FormatUuid(const std::span<const std::uint8_t> bytes)
        {
            std::string text;
            text.reserve(36);
            for (std::size_t i = 0; i < 16; ++i)
            {
                text += std::format("{:02x}", static_cast<unsigned>(bytes[i]));
                if (i == 3 || i == 5 || i == 7 || i == 9)
                {
                    text += '-';
                }
            }
            return text;
        }
    }

    bool ReadVarint(N2Net::ByteReader &reader, std::uint64_t &value) noexcept
    {
        value = 0;
        for (int i = 0; i < 10; ++i)
        {
            const std::uint8_t byte = reader.U8();
            if (reader.Failed())
            {
                return false;
            }
            if (i == 9 && byte > 1)
            {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7Fu) << (7 * i);
            if ((byte & 0x80u) == 0)
            {
                // A canonical varint never ends in a zero byte unless it is the only byte
                return !(i > 0 && byte == 0);
            }
        }
        return false;
    }

    bool ReadZigZag(N2Net::ByteReader &reader, std::int64_t &value) noexcept
    {
        std::uint64_t raw = 0;
        if (!ReadVarint(reader, raw))
        {
            return false;
        }
        const std::uint64_t negative = (raw & 1u) != 0 ? ~static_cast<std::uint64_t>(0) : 0;
        value = static_cast<std::int64_t>((raw >> 1) ^ negative);
        return true;
    }

    std::size_t MaxEncodedBytes(const FieldInfo &field) noexcept
    {
        switch (field.kind)
        {
        case FieldKind::Bool:
            return 1;
        case FieldKind::Int:
            return 10;
        case FieldKind::Float:
            return field.replicationStep > 0.0 ? 5 : (UsesRange(field) ? 2 : 4);
        case FieldKind::String:
            return 2 + kMaxReplicatedStringBytes;
        case FieldKind::Vector2:
            return 2 * AxisBytes(field);
        case FieldKind::Vector3:
            return 3 * AxisBytes(field);
        case FieldKind::Vector4:
            return 4 * AxisBytes(field);
        case FieldKind::Quaternion:
            return 4;
        case FieldKind::Color:
            return 4;
        case FieldKind::Enum:
            return 5;
        case FieldKind::AssetRef:
            return 17;
        case FieldKind::GameObjectRef:
            return 5;
        default:
            return 0;
        }
    }

    bool EncodeField(const FieldInfo &field, const json &value, const IReplicationReferences *references,
                     std::vector<std::uint8_t> &out)
    {
        if (!IsReplicableKind(field.kind))
        {
            return false;
        }
        const std::size_t restore = out.size();
        WireWriter writer(out);
        bool ok = false;
        switch (field.kind)
        {
        case FieldKind::Bool:
            if (value.is_boolean())
            {
                writer.U8(value.get<bool>() ? 1 : 0);
                ok = true;
            }
            break;
        case FieldKind::Int:
            if (value.is_number_integer() || value.is_number_unsigned())
            {
                if (value.is_number_unsigned() &&
                    value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                {
                    break;
                }
                writer.ZigZag(value.get<std::int64_t>());
                ok = true;
            }
            break;
        case FieldKind::Float:
            if (value.is_number())
            {
                const double number = value.get<double>();
                if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
                {
                    break;
                }
                if (field.replicationStep <= 0.0 && UsesRange(field))
                {
                    const double low = field.range->first;
                    const double high = field.range->second;
                    const double fraction = (std::clamp(number, low, high) - low) / (high - low);
                    writer.U16(static_cast<std::uint16_t>(std::clamp(std::lround(fraction * 65535.0), 0L, 65535L)));
                }
                else
                {
                    PutAxis(field, number, writer);
                }
                ok = true;
            }
            break;
        case FieldKind::Vector2:
        case FieldKind::Vector3:
        case FieldKind::Vector4:
        {
            std::vector<const char *> keys = {"x", "y"};
            if (field.kind == FieldKind::Vector3)
            {
                keys = {"x", "y", "z"};
            }
            else if (field.kind == FieldKind::Vector4)
            {
                keys = {"w", "x", "y", "z"};
            }
            if (!value.is_object())
            {
                break;
            }
            std::array<double, 4> axes{};
            bool numbers = true;
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                numbers = numbers && GetNumber(value, keys[i], axes[i]);
            }
            if (!numbers)
            {
                break;
            }
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                PutAxis(field, axes[i], writer);
            }
            ok = true;
            break;
        }
        case FieldKind::Quaternion:
            ok = EncodeQuaternion(value, writer);
            break;
        case FieldKind::Color:
        {
            if (!value.is_object())
            {
                break;
            }
            bool numbers = true;
            std::array<std::uint8_t, 4> channels{};
            const std::vector<const char *> keys = ColorKeys(field);
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                double channel = 0.0;
                numbers = numbers && GetNumber(value, keys[i], channel);
                channels[i] = ToByte(channel);
            }
            if (!numbers)
            {
                break;
            }
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                writer.U8(channels[i]);
            }
            ok = true;
            break;
        }
        case FieldKind::String:
            if (value.is_string())
            {
                const std::string &text = value.get_ref<const std::string &>();
                if (!IsValidUtf8(text))
                {
                    break;
                }
                std::size_t length = std::min(text.size(), kMaxReplicatedStringBytes);
                if (length < text.size())
                {
                    // Cut at a character boundary: back up over the continuation bytes of the character that straddles it
                    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
                    {
                        --length;
                    }
                }
                writer.Varint(length);
                writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), length));
                ok = true;
            }
            break;
        case FieldKind::Enum:
            if (value.is_string())
            {
                const auto found = std::find(field.enumOptions.begin(), field.enumOptions.end(), value.get<std::string>());
                if (found != field.enumOptions.end())
                {
                    writer.Varint(static_cast<std::uint64_t>(found - field.enumOptions.begin()));
                    ok = true;
                }
            }
            break;
        case FieldKind::AssetRef:
            if (value.is_null())
            {
                writer.U8(0);
                ok = true;
            }
            else if (value.is_string())
            {
                const std::optional<Math::UUID> uuid = Math::UUID::FromString(value.get<std::string>());
                if (!uuid.has_value())
                {
                    break;
                }
                if (*uuid == Math::UUID::ZERO)
                {
                    writer.U8(0);
                }
                else
                {
                    writer.U8(1);
                    writer.Bytes(std::span<const std::uint8_t>(uuid->GetBytes().data(), 16));
                }
                ok = true;
            }
            break;
        case FieldKind::GameObjectRef:
            if (value.is_null())
            {
                writer.Varint(0);
                ok = true;
            }
            else if (value.is_string())
            {
                const std::uint32_t id = references != nullptr ? references->NetIdOf(value.get<std::string>()) : 0;
                writer.Varint(id);
                ok = true;
            }
            break;
        default:
            break;
        }
        if (!ok)
        {
            out.resize(restore);
        }
        return ok;
    }

    DecodeStatus DecodeField(const FieldInfo &field, N2Net::ByteReader &reader, const IReplicationReferences *references,
                             json &value)
    {
        if (!IsReplicableKind(field.kind))
        {
            return DecodeStatus::Malformed;
        }
        json decoded;
        switch (field.kind)
        {
        case FieldKind::Bool:
        {
            const std::uint8_t byte = reader.U8();
            if (reader.Failed() || byte > 1)
            {
                return DecodeStatus::Malformed;
            }
            decoded = byte == 1;
            break;
        }
        case FieldKind::Int:
        {
            std::int64_t number = 0;
            if (!ReadZigZag(reader, number))
            {
                return DecodeStatus::Malformed;
            }
            decoded = number;
            break;
        }
        case FieldKind::Float:
        {
            if (field.replicationStep <= 0.0 && UsesRange(field))
            {
                const std::uint16_t raw = reader.U16();
                if (reader.Failed())
                {
                    return DecodeStatus::Malformed;
                }
                const double low = field.range->first;
                const double high = field.range->second;
                decoded = low + static_cast<double>(raw) / 65535.0 * (high - low);
            }
            else
            {
                double number = 0.0;
                if (!GetAxis(field, reader, number))
                {
                    return DecodeStatus::Malformed;
                }
                decoded = number;
            }
            break;
        }
        case FieldKind::Vector2:
        case FieldKind::Vector3:
        case FieldKind::Vector4:
        {
            std::array<double, 4> axes{};
            const std::size_t count = field.kind == FieldKind::Vector2 ? 2 : (field.kind == FieldKind::Vector3 ? 3 : 4);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!GetAxis(field, reader, axes[i]))
                {
                    return DecodeStatus::Malformed;
                }
            }
            if (field.kind == FieldKind::Vector2)
            {
                decoded = json{{"x", axes[0]}, {"y", axes[1]}};
            }
            else if (field.kind == FieldKind::Vector3)
            {
                decoded = json{{"x", axes[0]}, {"y", axes[1]}, {"z", axes[2]}};
            }
            else
            {
                decoded = json{{"w", axes[0]}, {"x", axes[1]}, {"y", axes[2]}, {"z", axes[3]}};
            }
            break;
        }
        case FieldKind::Quaternion:
            if (!DecodeQuaternion(reader, decoded))
            {
                return DecodeStatus::Malformed;
            }
            break;
        case FieldKind::Color:
        {
            const std::vector<const char *> keys = ColorKeys(field);
            decoded = json::object();
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                const std::uint8_t byte = reader.U8();
                if (reader.Failed())
                {
                    return DecodeStatus::Malformed;
                }
                decoded[keys[i]] = static_cast<double>(byte) / 255.0;
            }
            break;
        }
        case FieldKind::String:
        {
            std::uint64_t length = 0;
            if (!ReadVarint(reader, length) || length > kMaxReplicatedStringBytes || length > reader.Remaining())
            {
                return DecodeStatus::Malformed;
            }
            const std::span<const std::uint8_t> bytes = reader.Bytes(static_cast<std::size_t>(length));
            if (reader.Failed())
            {
                return DecodeStatus::Malformed;
            }
            std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            if (!IsValidUtf8(text))
            {
                return DecodeStatus::Malformed;
            }
            decoded = std::move(text);
            break;
        }
        case FieldKind::Enum:
        {
            std::uint64_t index = 0;
            if (!ReadVarint(reader, index) || index >= field.enumOptions.size())
            {
                return DecodeStatus::Malformed;
            }
            decoded = field.enumOptions[static_cast<std::size_t>(index)];
            break;
        }
        case FieldKind::AssetRef:
        {
            const std::uint8_t present = reader.U8();
            if (reader.Failed() || present > 1)
            {
                return DecodeStatus::Malformed;
            }
            if (present == 0)
            {
                decoded = nullptr;
            }
            else
            {
                const std::span<const std::uint8_t> bytes = reader.Bytes(16);
                if (reader.Failed())
                {
                    return DecodeStatus::Malformed;
                }
                decoded = FormatUuid(bytes);
            }
            break;
        }
        case FieldKind::GameObjectRef:
        {
            std::uint64_t id = 0;
            if (!ReadVarint(reader, id) || id > std::numeric_limits<std::uint32_t>::max())
            {
                return DecodeStatus::Malformed;
            }
            if (id == 0)
            {
                decoded = nullptr;
            }
            else
            {
                const std::optional<std::string> uuid =
                    references != nullptr ? references->UuidOf(static_cast<std::uint32_t>(id)) : std::nullopt;
                if (!uuid.has_value())
                {
                    return DecodeStatus::UnknownReference;
                }
                decoded = *uuid;
            }
            break;
        }
        default:
            return DecodeStatus::Malformed;
        }

        // The field's own checks come after the wire's: a value of the right shape that the field still refuses
        if (ValidateFieldValue(field, decoded).has_value())
        {
            return DecodeStatus::Malformed;
        }
        value = ClampFieldValue(field, decoded);
        return DecodeStatus::Ok;
    }

    void HashFieldFormat(Fnv1a64 &hash, const FieldInfo &field)
    {
        hash.String(field.name);
        hash.String(field.container);
        hash.U8(static_cast<std::uint8_t>(field.kind));
        hash.String(field.typeName);
        hash.U64(std::bit_cast<std::uint64_t>(field.replicationStep));
        const bool ranged = UsesRange(field);
        hash.U8(ranged ? 1 : 0);
        if (ranged)
        {
            hash.U64(std::bit_cast<std::uint64_t>(field.range->first));
            hash.U64(std::bit_cast<std::uint64_t>(field.range->second));
        }
        hash.U32(static_cast<std::uint32_t>(field.enumOptions.size()));
        for (const std::string &option : field.enumOptions)
        {
            hash.String(option);
        }
    }
}
