#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Logger.hpp"
#include "engine/network/NetworkHash.hpp"
#include "engine/network/ReplicationCodec.hpp"
#include "engine/network/n2net/ByteIO.hpp"
#include "engine/network/n2net/SimRandom.hpp"
#include "engine/serialization/FieldInfo.hpp"

// The wire codecs of replicated fields (netcode stage B2): an exact round trip where the encoding is exact, a stated
// error bound where it quantises, the same value always the same bytes, and a decoder that takes only what the bytes
// and its caps allow, whatever it is given.

using namespace N2Engine;
using namespace N2Engine::Network;
using json = nlohmann::json;

namespace
{
    FieldInfo MakeField(const FieldKind kind, const std::string &typeName = std::string())
    {
        FieldInfo field;
        field.name = "value";
        field.kind = kind;
        field.typeName = typeName;
        field.replicated = true;
        field.limits = {-9.0e18, 9.0e18};
        return field;
    }

    /// GameObject references by a table of uuid text and net id
    class FakeReferences final : public IReplicationReferences
    {
    public:
        std::map<std::string, std::uint32_t> ids;

        [[nodiscard]] std::uint32_t NetIdOf(const std::string &uuid) const override
        {
            const auto found = ids.find(uuid);
            return found == ids.end() ? 0 : found->second;
        }

        [[nodiscard]] std::optional<std::string> UuidOf(const std::uint32_t netId) const override
        {
            for (const auto &entry : ids)
            {
                if (entry.second == netId)
                {
                    return entry.first;
                }
            }
            return std::nullopt;
        }
    };

    std::vector<std::uint8_t> EncodeOk(const FieldInfo &field, const json &value,
                                       const IReplicationReferences *references = nullptr)
    {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(EncodeField(field, value, references, bytes)) << value.dump();
        return bytes;
    }

    json DecodeOk(const FieldInfo &field, const std::vector<std::uint8_t> &bytes,
                  const IReplicationReferences *references = nullptr)
    {
        N2Net::ByteReader reader(bytes);
        json value;
        EXPECT_EQ(DecodeField(field, reader, references, value), DecodeStatus::Ok);
        EXPECT_EQ(reader.Remaining(), 0u) << "the decoder left bytes unread";
        return value;
    }

    json RoundTrip(const FieldInfo &field, const json &value, const IReplicationReferences *references = nullptr)
    {
        return DecodeOk(field, EncodeOk(field, value, references), references);
    }

    json Vec3(const double x, const double y, const double z)
    {
        return json{{"x", x}, {"y", y}, {"z", z}};
    }

    json Quat(const double w, const double x, const double y, const double z)
    {
        return json{{"w", w}, {"x", x}, {"y", y}, {"z", z}};
    }

    double Dot(const json &a, const json &b)
    {
        return a.at("w").get<double>() * b.at("w").get<double>() + a.at("x").get<double>() * b.at("x").get<double>() +
               a.at("y").get<double>() * b.at("y").get<double>() + a.at("z").get<double>() * b.at("z").get<double>();
    }

    DecodeStatus TryDecode(const FieldInfo &field, const std::vector<std::uint8_t> &bytes,
                           const IReplicationReferences *references = nullptr)
    {
        N2Net::ByteReader reader(bytes);
        json value;
        return DecodeField(field, reader, references, value);
    }
}

TEST(ReplicationCodecTest, VarintsAreCanonicalAndBounded)
{
    for (const std::uint64_t value : {0ull, 1ull, 127ull, 128ull, 300ull, 16383ull, 16384ull, 0xFFFFFFFFull,
                                      0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull})
    {
        std::vector<std::uint8_t> bytes;
        WireWriter(bytes).Varint(value);
        EXPECT_LE(bytes.size(), 10u);
        N2Net::ByteReader reader(bytes);
        std::uint64_t decoded = 0;
        ASSERT_TRUE(ReadVarint(reader, decoded)) << value;
        EXPECT_EQ(decoded, value);
        EXPECT_EQ(reader.Remaining(), 0u);
    }

    // Not canonical: a trailing zero byte (0 written in two bytes), too long, overflowing 64 bits, cut short
    const std::vector<std::vector<std::uint8_t>> bad = {
        {0x80, 0x00},
        {0xFF, 0x00},
        {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02},
        {0x80},
        {},
    };
    for (const std::vector<std::uint8_t> &bytes : bad)
    {
        N2Net::ByteReader reader(bytes);
        std::uint64_t decoded = 0;
        EXPECT_FALSE(ReadVarint(reader, decoded)) << bytes.size() << " bytes";
    }
}

TEST(ReplicationCodecTest, ZigZagRoundTripsTheExtremes)
{
    for (const std::int64_t value : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{1}, std::int64_t{-2},
                                     std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()})
    {
        std::vector<std::uint8_t> bytes;
        WireWriter(bytes).ZigZag(value);
        N2Net::ByteReader reader(bytes);
        std::int64_t decoded = 1234;
        ASSERT_TRUE(ReadZigZag(reader, decoded));
        EXPECT_EQ(decoded, value);
    }
    // Small magnitudes are short: that is the point
    std::vector<std::uint8_t> bytes;
    WireWriter(bytes).ZigZag(-3);
    EXPECT_EQ(bytes.size(), 1u);
}

TEST(ReplicationCodecTest, BoolAndIntRoundTripExactly)
{
    const FieldInfo boolean = MakeField(FieldKind::Bool);
    EXPECT_EQ(RoundTrip(boolean, true), json(true));
    EXPECT_EQ(RoundTrip(boolean, false), json(false));
    EXPECT_EQ(EncodeOk(boolean, true).size(), 1u);

    const FieldInfo integer = MakeField(FieldKind::Int);
    for (const std::int64_t value : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{123456789},
                                     std::int64_t{-987654321012345}, std::int64_t{8'000'000'000'000'000'000}})
    {
        const json decoded = RoundTrip(integer, value);
        EXPECT_TRUE(decoded.is_number_integer());
        EXPECT_EQ(decoded.get<std::int64_t>(), value);
    }
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(EncodeField(integer, json(2.5), nullptr, bytes)) << "an Int field takes a whole number";
    EXPECT_FALSE(EncodeField(integer, json("7"), nullptr, bytes));
    EXPECT_TRUE(bytes.empty()) << "a refused value leaves nothing behind";
}

TEST(ReplicationCodecTest, FloatsAreExactAsFloat32OrBoundedByTheirQuantisation)
{
    // The full float: the float32 value, exactly
    const FieldInfo plain = MakeField(FieldKind::Float);
    EXPECT_EQ(EncodeOk(plain, 1.5).size(), 4u);
    EXPECT_DOUBLE_EQ(RoundTrip(plain, 1.5).get<double>(), 1.5);
    EXPECT_DOUBLE_EQ(RoundTrip(plain, -0.25).get<double>(), -0.25);
    EXPECT_FLOAT_EQ(static_cast<float>(RoundTrip(plain, 0.1).get<double>()), 0.1f);

    // A Range: 16 bits across it, at most half a step off, and the ends are exact
    FieldInfo ranged = MakeField(FieldKind::Float);
    ranged.range = std::make_pair(-10.0, 30.0);
    EXPECT_EQ(EncodeOk(ranged, 5.0).size(), 2u);
    const double step = 40.0 / 65535.0;
    for (const double value : {-10.0, 30.0, 0.0, 12.345, -9.99, 29.999})
    {
        EXPECT_NEAR(RoundTrip(ranged, value).get<double>(), value, step / 2.0 + 1e-9) << value;
    }
    EXPECT_DOUBLE_EQ(RoundTrip(ranged, -10.0).get<double>(), -10.0);
    EXPECT_DOUBLE_EQ(RoundTrip(ranged, 30.0).get<double>(), 30.0);
    // Outside the range it is clamped to it
    EXPECT_DOUBLE_EQ(RoundTrip(ranged, 99.0).get<double>(), 30.0);

    // A step: whole steps as a varint, at most half a step off, and a change under half a step does not change the bytes
    FieldInfo stepped = MakeField(FieldKind::Float);
    stepped.replicationStep = 0.01;
    for (const double value : {0.0, 0.004, 0.006, -0.006, 1.234, -250.5, 12345.678})
    {
        EXPECT_NEAR(RoundTrip(stepped, value).get<double>(), value, 0.005 + 1e-9) << value;
    }
    EXPECT_EQ(EncodeOk(stepped, 1.0001), EncodeOk(stepped, 1.0004));
    EXPECT_NE(EncodeOk(stepped, 1.0), EncodeOk(stepped, 1.02));
    EXPECT_EQ(EncodeOk(stepped, 0.0).size(), 1u);

    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(EncodeField(plain, json(std::numeric_limits<double>::infinity()), nullptr, bytes));
    EXPECT_FALSE(EncodeField(plain, json(1.0e300), nullptr, bytes)) << "more than a float holds";
    EXPECT_FALSE(EncodeField(plain, json(true), nullptr, bytes));
}

TEST(ReplicationCodecTest, AStepCountPastThirtyTwoBitsIsClampedAndWarnedOncePerField)
{
    std::vector<std::string> warnings;
    const std::size_t subscription = Logger::logEvent += [&warnings](const std::string_view text, const Logger::LogLevel level)
    {
        if (level == Logger::LogLevel::Warn && text.find("ReplicationCodecTest_clamped") != std::string_view::npos)
        {
            warnings.emplace_back(text);
        }
    };

    // The review's case: a step of 0.0001 and x = 300000 is 3e9 steps, over 2^31 - 1
    FieldInfo stepped = MakeField(FieldKind::Float);
    stepped.name = "ReplicationCodecTest_clamped";
    stepped.replicationStep = 0.0001;
    const double clampedValue = 2147483647.0 * 0.0001;
    EXPECT_NEAR(RoundTrip(stepped, 300000.0).get<double>(), clampedValue, 1e-6);
    EXPECT_NEAR(RoundTrip(stepped, -300000.0).get<double>(), -clampedValue, 1e-6);
    EXPECT_NEAR(RoundTrip(stepped, 250000.0).get<double>(), clampedValue, 1e-6);
    ASSERT_EQ(warnings.size(), 1u) << "one warning for the field, not one per value sent";
    EXPECT_NE(warnings[0].find("clamped"), std::string::npos) << warnings[0];

    // A value that fits says nothing; a vector axis of another field (another name) is warned once on its own
    EXPECT_NEAR(RoundTrip(stepped, 1000.0).get<double>(), 1000.0, 0.00005 + 1e-9);
    FieldInfo vector = MakeField(FieldKind::Vector3);
    vector.name = "ReplicationCodecTest_clamped_vector";
    vector.replicationStep = 0.0001;
    (void)RoundTrip(vector, Vec3(300000.0, 300000.0, 0.0));
    (void)RoundTrip(vector, Vec3(0.0, -300000.0, 0.0));
    EXPECT_EQ(warnings.size(), 2u);

    Logger::logEvent -= subscription;
}

TEST(ReplicationCodecTest, VectorsRoundTripAndHonourTheirStep)
{
    const FieldInfo v2 = MakeField(FieldKind::Vector2);
    const json two = json{{"x", 1.5}, {"y", -2.25}};
    EXPECT_EQ(RoundTrip(v2, two), two);
    EXPECT_EQ(EncodeOk(v2, two).size(), 8u);

    const FieldInfo v3 = MakeField(FieldKind::Vector3);
    const json three = Vec3(1.0, -2.0, 3.5);
    EXPECT_EQ(RoundTrip(v3, three), three);
    EXPECT_EQ(EncodeOk(v3, three).size(), 12u);

    const FieldInfo v4 = MakeField(FieldKind::Vector4);
    const json four = json{{"w", 4.0}, {"x", 1.0}, {"y", 2.0}, {"z", 3.0}};
    EXPECT_EQ(RoundTrip(v4, four), four);
    EXPECT_EQ(EncodeOk(v4, four).size(), 16u);

    FieldInfo stepped = MakeField(FieldKind::Vector3);
    stepped.replicationStep = 0.05;
    for (const json &value : {Vec3(0.0, 0.0, 0.0), Vec3(10.123, -20.456, 30.789), Vec3(-0.02, 0.02, 1000.0)})
    {
        const json decoded = RoundTrip(stepped, value);
        for (const char *axis : {"x", "y", "z"})
        {
            EXPECT_NEAR(decoded.at(axis).get<double>(), value.at(axis).get<double>(), 0.025 + 1e-9) << axis;
        }
    }
    EXPECT_EQ(EncodeOk(stepped, Vec3(0.0, 0.0, 0.0)).size(), 3u) << "a position at rest costs a byte per axis";

    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(EncodeField(v3, json{{"x", 1.0}, {"y", 2.0}}, nullptr, bytes)) << "a missing axis";
    EXPECT_FALSE(EncodeField(v3, json(5), nullptr, bytes));
}

TEST(ReplicationCodecTest, QuaternionsKeepTheirRotationWithinTheBound)
{
    const FieldInfo field = MakeField(FieldKind::Quaternion);
    std::vector<json> samples = {Quat(1, 0, 0, 0), Quat(0, 1, 0, 0), Quat(0, 0, 1, 0), Quat(0, 0, 0, 1),
                                 Quat(-1, 0, 0, 0), Quat(0.5, 0.5, 0.5, 0.5), Quat(-0.5, 0.5, -0.5, 0.5),
                                 Quat(0.9238795, 0.3826834, 0, 0), Quat(0.1, -0.7, 0.3, 0.64)};
    // Every one of the four components is the largest in some sample, with both signs
    N2Net::SimRandom random(7);
    for (int i = 0; i < 200; ++i)
    {
        const double w = static_cast<double>(random.NextBelow(2001)) / 1000.0 - 1.0;
        const double x = static_cast<double>(random.NextBelow(2001)) / 1000.0 - 1.0;
        const double y = static_cast<double>(random.NextBelow(2001)) / 1000.0 - 1.0;
        const double z = static_cast<double>(random.NextBelow(2001)) / 1000.0 - 1.0;
        if (w * w + x * x + y * y + z * z > 0.01)
        {
            samples.push_back(Quat(w, x, y, z));
        }
    }
    for (const json &sample : samples)
    {
        const std::vector<std::uint8_t> bytes = EncodeOk(field, sample);
        EXPECT_EQ(bytes.size(), 4u);
        const json decoded = DecodeOk(field, bytes);
        const double length = std::sqrt(Dot(sample, sample));
        json unit = sample;
        for (const char *key : {"w", "x", "y", "z"})
        {
            unit[key] = sample.at(key).get<double>() / length;
        }
        // q and -q are the same rotation: the angle between them is 2 acos |dot|
        const double dot = std::min(1.0, std::fabs(Dot(unit, decoded)));
        EXPECT_LT(2.0 * std::acos(dot), 0.005) << sample.dump();
        EXPECT_NEAR(std::sqrt(Dot(decoded, decoded)), 1.0, 0.002);
    }
    // Deterministic: the same rotation, the same bytes; q and -q too
    EXPECT_EQ(EncodeOk(field, Quat(0.5, 0.5, 0.5, 0.5)), EncodeOk(field, Quat(-0.5, -0.5, -0.5, -0.5)));
    // Not a rotation at all (zero length) is the identity
    EXPECT_NEAR(RoundTrip(field, Quat(0, 0, 0, 0)).at("w").get<double>(), 1.0, 0.002);
}

TEST(ReplicationCodecTest, ColorsAreEightBitsPerChannel)
{
    const FieldInfo rgba = MakeField(FieldKind::Color, "Color");
    const json color = json{{"r", 1.0}, {"g", 0.5}, {"b", 0.0}, {"a", 0.25}};
    const json decoded = RoundTrip(rgba, color);
    EXPECT_EQ(EncodeOk(rgba, color).size(), 4u);
    EXPECT_DOUBLE_EQ(decoded.at("r").get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(decoded.at("b").get<double>(), 0.0);
    EXPECT_NEAR(decoded.at("g").get<double>(), 0.5, 1.0 / 510.0 + 1e-9);
    EXPECT_NEAR(decoded.at("a").get<double>(), 0.25, 1.0 / 510.0 + 1e-9);
    // Beyond [0, 1] is clamped
    const json hdr = json{{"r", 4.0}, {"g", -1.0}, {"b", 0.0}, {"a", 1.0}};
    EXPECT_DOUBLE_EQ(RoundTrip(rgba, hdr).at("r").get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(RoundTrip(rgba, hdr).at("g").get<double>(), 0.0);

    // A Vector3 marked as a colour travels as x, y, z (three bytes); a Vector4 as w, x, y, z
    const FieldInfo asVec3 = MakeField(FieldKind::Color, "Vector3");
    const json v3 = Vec3(0.2, 0.4, 0.6);
    EXPECT_EQ(EncodeOk(asVec3, v3).size(), 3u);
    EXPECT_NEAR(RoundTrip(asVec3, v3).at("y").get<double>(), 0.4, 1.0 / 510.0 + 1e-9);
    const FieldInfo asVec4 = MakeField(FieldKind::Color, "Vector4");
    const json v4 = json{{"w", 1.0}, {"x", 0.0}, {"y", 1.0}, {"z", 0.0}};
    EXPECT_EQ(EncodeOk(asVec4, v4).size(), 4u);
    EXPECT_DOUBLE_EQ(RoundTrip(asVec4, v4).at("w").get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(RoundTrip(asVec4, v4).at("x").get<double>(), 0.0);
}

TEST(ReplicationCodecTest, StringsAreCappedAndMustBeUtf8)
{
    const FieldInfo field = MakeField(FieldKind::String);
    EXPECT_EQ(RoundTrip(field, "").get<std::string>(), "");
    EXPECT_EQ(RoundTrip(field, "hello").get<std::string>(), "hello");
    const std::string accents = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";
    EXPECT_EQ(RoundTrip(field, accents).get<std::string>(), accents);

    // At the cap exactly: whole; over it: cut
    const std::string atCap(kMaxReplicatedStringBytes, 'a');
    EXPECT_EQ(RoundTrip(field, atCap).get<std::string>(), atCap);
    const std::string over(kMaxReplicatedStringBytes + 40, 'b');
    EXPECT_EQ(RoundTrip(field, over).get<std::string>(), std::string(kMaxReplicatedStringBytes, 'b'));
    EXPECT_LE(EncodeOk(field, over).size(), MaxEncodedBytes(field));

    // A cut never splits a character: a three byte character straddling the cap is dropped whole
    std::string straddle(kMaxReplicatedStringBytes - 1, 'c');
    straddle += "\xE2\x82\xAC";
    const std::string cut = RoundTrip(field, straddle).get<std::string>();
    EXPECT_EQ(cut, std::string(kMaxReplicatedStringBytes - 1, 'c'));

    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(EncodeField(field, json(std::string("bad\xFF")), nullptr, bytes)) << "invalid UTF-8";
    EXPECT_FALSE(EncodeField(field, json(std::string("\xC0\x80")), nullptr, bytes)) << "overlong form";
    EXPECT_FALSE(EncodeField(field, json(std::string("\xED\xA0\x80")), nullptr, bytes)) << "a surrogate";
    EXPECT_FALSE(EncodeField(field, json(3), nullptr, bytes));
}

TEST(ReplicationCodecTest, EnumsAssetsAndObjectReferences)
{
    FieldInfo enumeration = MakeField(FieldKind::Enum);
    enumeration.enumOptions = {"Red", "Green", "Blue"};
    EXPECT_EQ(RoundTrip(enumeration, "Blue").get<std::string>(), "Blue");
    EXPECT_EQ(RoundTrip(enumeration, "Red").get<std::string>(), "Red");
    EXPECT_EQ(EncodeOk(enumeration, "Green").size(), 1u);
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(EncodeField(enumeration, json("Purple"), nullptr, bytes));

    const FieldInfo asset = MakeField(FieldKind::AssetRef);
    EXPECT_TRUE(RoundTrip(asset, nullptr).is_null());
    EXPECT_EQ(EncodeOk(asset, nullptr).size(), 1u);
    const std::string uuid = "12345678-9abc-4def-8123-456789abcdef";
    EXPECT_EQ(RoundTrip(asset, uuid).get<std::string>(), uuid);
    EXPECT_EQ(EncodeOk(asset, uuid).size(), 17u);
    EXPECT_TRUE(RoundTrip(asset, "00000000-0000-0000-0000-000000000000").is_null()) << "the zero uuid is none";
    EXPECT_FALSE(EncodeField(asset, json("not a uuid"), nullptr, bytes));

    const FieldInfo object = MakeField(FieldKind::GameObjectRef);
    FakeReferences references;
    references.ids[uuid] = 77;
    EXPECT_EQ(RoundTrip(object, uuid, &references).get<std::string>(), uuid);
    EXPECT_EQ(EncodeOk(object, uuid, &references).size(), 1u);
    EXPECT_TRUE(RoundTrip(object, nullptr, &references).is_null());
    // An object with no net id is sent as none; a net id this side has no object for is not applicable
    EXPECT_TRUE(RoundTrip(object, "aaaaaaaa-9abc-4def-8123-456789abcdef", &references).is_null());
    std::vector<std::uint8_t> unknown;
    WireWriter(unknown).Varint(999);
    EXPECT_EQ(TryDecode(object, unknown, &references), DecodeStatus::UnknownReference);
    EXPECT_EQ(TryDecode(object, unknown, nullptr), DecodeStatus::UnknownReference);
}

TEST(ReplicationCodecTest, KindsThatCannotBeReplicatedAreRefused)
{
    for (const FieldKind kind : {FieldKind::AssetRefList, FieldKind::GameObjectRefList, FieldKind::ComponentRef,
                                 FieldKind::ComponentRefList, FieldKind::Json})
    {
        EXPECT_FALSE(IsReplicableKind(kind)) << FieldKindName(kind);
        const FieldInfo field = MakeField(kind);
        EXPECT_EQ(MaxEncodedBytes(field), 0u);
        std::vector<std::uint8_t> bytes;
        EXPECT_FALSE(EncodeField(field, json::array(), nullptr, bytes));
        EXPECT_EQ(TryDecode(field, {1, 2, 3}), DecodeStatus::Malformed);
    }
    for (const FieldKind kind : {FieldKind::Bool, FieldKind::Int, FieldKind::Float, FieldKind::String, FieldKind::Vector2,
                                 FieldKind::Vector3, FieldKind::Vector4, FieldKind::Quaternion, FieldKind::Color,
                                 FieldKind::Enum, FieldKind::AssetRef, FieldKind::GameObjectRef})
    {
        EXPECT_TRUE(IsReplicableKind(kind)) << FieldKindName(kind);
        EXPECT_GT(MaxEncodedBytes(MakeField(kind)), 0u);
    }
}

TEST(ReplicationCodecTest, MaxEncodedBytesIsNeverExceeded)
{
    FieldInfo ranged = MakeField(FieldKind::Float);
    ranged.range = std::make_pair(0.0, 1.0);
    FieldInfo stepped3 = MakeField(FieldKind::Vector3);
    stepped3.replicationStep = 1e-9;
    FieldInfo enumeration = MakeField(FieldKind::Enum);
    enumeration.enumOptions = {"A", "B"};
    const std::vector<std::pair<FieldInfo, json>> cases = {
        {MakeField(FieldKind::Bool), true},
        {MakeField(FieldKind::Int), std::numeric_limits<std::int64_t>::min()},
        {MakeField(FieldKind::Int), std::numeric_limits<std::int64_t>::max()},
        {MakeField(FieldKind::Float), 1.0e30},
        {ranged, 0.5},
        {stepped3, Vec3(3.0e9, -3.0e9, 1.0)},
        {MakeField(FieldKind::Vector4), json{{"w", 1.0}, {"x", 1.0}, {"y", 1.0}, {"z", 1.0}}},
        {MakeField(FieldKind::Quaternion), Quat(1, 0, 0, 0)},
        {MakeField(FieldKind::Color, "Color"), json{{"r", 1.0}, {"g", 1.0}, {"b", 1.0}, {"a", 1.0}}},
        {MakeField(FieldKind::String), std::string(1000, 'z')},
        {enumeration, "B"},
        {MakeField(FieldKind::AssetRef), "12345678-9abc-4def-8123-456789abcdef"},
    };
    for (const auto &entry : cases)
    {
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(EncodeField(entry.first, entry.second, nullptr, bytes)) << FieldKindName(entry.first.kind);
        EXPECT_LE(bytes.size(), MaxEncodedBytes(entry.first)) << FieldKindName(entry.first.kind);
    }
}

TEST(ReplicationCodecTest, SameValueSameBytes)
{
    FieldInfo stepped = MakeField(FieldKind::Vector3);
    stepped.replicationStep = 0.01;
    const json value = Vec3(1.2345, -6.789, 100.001);
    const std::vector<std::uint8_t> first = EncodeOk(stepped, value);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(EncodeOk(stepped, value), first);
    }
    // Encoding appends: two fields back to back are the two encodings
    std::vector<std::uint8_t> both;
    ASSERT_TRUE(EncodeField(stepped, value, nullptr, both));
    ASSERT_TRUE(EncodeField(stepped, value, nullptr, both));
    EXPECT_EQ(both.size(), first.size() * 2);
    EXPECT_TRUE(std::equal(first.begin(), first.end(), both.begin()));
    EXPECT_TRUE(std::equal(first.begin(), first.end(), both.begin() + static_cast<std::ptrdiff_t>(first.size())));
}

TEST(ReplicationCodecTest, EveryTruncationIsMalformedNeverAReadPastTheEnd)
{
    FieldInfo enumeration = MakeField(FieldKind::Enum);
    enumeration.enumOptions = {"A", "B", "C"};
    FieldInfo stepped = MakeField(FieldKind::Vector3);
    stepped.replicationStep = 0.5;
    FieldInfo ranged = MakeField(FieldKind::Float);
    ranged.range = std::make_pair(0.0, 10.0);
    const std::vector<std::pair<FieldInfo, json>> cases = {
        {MakeField(FieldKind::Int), 1'000'000'000'000ll},
        {MakeField(FieldKind::Float), 3.5},
        {ranged, 3.5},
        {MakeField(FieldKind::Vector3), Vec3(1, 2, 3)},
        {stepped, Vec3(1000, 2000, 3000)},
        {MakeField(FieldKind::Vector4), json{{"w", 1.0}, {"x", 2.0}, {"y", 3.0}, {"z", 4.0}}},
        {MakeField(FieldKind::Quaternion), Quat(0.5, 0.5, 0.5, 0.5)},
        {MakeField(FieldKind::Color, "Color"), json{{"r", 1.0}, {"g", 0.5}, {"b", 0.0}, {"a", 1.0}}},
        {MakeField(FieldKind::String), "a string of some length"},
        {enumeration, "C"},
        {MakeField(FieldKind::AssetRef), "12345678-9abc-4def-8123-456789abcdef"},
    };
    for (const auto &entry : cases)
    {
        const std::vector<std::uint8_t> bytes = EncodeOk(entry.first, entry.second);
        ASSERT_GE(bytes.size(), 1u);
        for (std::size_t length = 0; length < bytes.size(); ++length)
        {
            const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
            EXPECT_EQ(TryDecode(entry.first, cut), DecodeStatus::Malformed)
                << FieldKindName(entry.first.kind) << " cut to " << length;
        }
    }
}

TEST(ReplicationCodecTest, AnythingOutOfRangeIsMalformed)
{
    // A bool that is not 0 or 1
    EXPECT_EQ(TryDecode(MakeField(FieldKind::Bool), {2}), DecodeStatus::Malformed);
    // A string longer than its cap, however many bytes follow (nothing is allocated for the claimed length)
    {
        std::vector<std::uint8_t> bytes;
        WireWriter(bytes).Varint(kMaxReplicatedStringBytes + 1);
        bytes.resize(bytes.size() + 4096, 'x');
        EXPECT_EQ(TryDecode(MakeField(FieldKind::String), bytes), DecodeStatus::Malformed);
        std::vector<std::uint8_t> huge;
        WireWriter(huge).Varint(0xFFFFFFFFFFull);
        EXPECT_EQ(TryDecode(MakeField(FieldKind::String), huge), DecodeStatus::Malformed);
    }
    // A string whose bytes are not UTF-8
    EXPECT_EQ(TryDecode(MakeField(FieldKind::String), {2, 0xC3, 0x28}), DecodeStatus::Malformed);
    // An enum index past the options, and into an enum with none
    FieldInfo enumeration = MakeField(FieldKind::Enum);
    enumeration.enumOptions = {"A", "B"};
    EXPECT_EQ(TryDecode(enumeration, {2}), DecodeStatus::Malformed);
    EXPECT_EQ(TryDecode(enumeration, {1}), DecodeStatus::Ok);
    EXPECT_EQ(TryDecode(MakeField(FieldKind::Enum), {0}), DecodeStatus::Malformed);
    // An asset reference with a presence byte that is neither
    EXPECT_EQ(TryDecode(MakeField(FieldKind::AssetRef), {2}), DecodeStatus::Malformed);
    // A float that is NaN or infinity
    EXPECT_EQ(TryDecode(MakeField(FieldKind::Float), {0x00, 0x00, 0xC0, 0x7F}), DecodeStatus::Malformed);
    EXPECT_EQ(TryDecode(MakeField(FieldKind::Float), {0x00, 0x00, 0x80, 0x7F}), DecodeStatus::Malformed);
    EXPECT_EQ(TryDecode(MakeField(FieldKind::Vector3), {0, 0, 0x80, 0x3F, 0, 0, 0x80, 0x3F, 0x00, 0x00, 0xC0, 0x7F}),
              DecodeStatus::Malformed);
    // A step count beyond 32 bits
    FieldInfo stepped = MakeField(FieldKind::Float);
    stepped.replicationStep = 0.1;
    std::vector<std::uint8_t> bigStep;
    WireWriter(bigStep).ZigZag(std::int64_t{1} << 40);
    EXPECT_EQ(TryDecode(stepped, bigStep), DecodeStatus::Malformed);
    // An int a C++ int member could not hold (the field's own limits decide)
    FieldInfo small = MakeField(FieldKind::Int);
    small.limits = {-2147483648.0, 2147483647.0};
    std::vector<std::uint8_t> bigInt;
    WireWriter(bigInt).ZigZag(std::int64_t{1} << 40);
    EXPECT_EQ(TryDecode(small, bigInt), DecodeStatus::Malformed);
    // A GameObject reference beyond 32 bits
    std::vector<std::uint8_t> bigId;
    WireWriter(bigId).Varint(std::uint64_t{1} << 40);
    EXPECT_EQ(TryDecode(MakeField(FieldKind::GameObjectRef), bigId), DecodeStatus::Malformed);
}

TEST(ReplicationCodecTest, RandomBytesNeverCrashAnyDecoder)
{
    FieldInfo enumeration = MakeField(FieldKind::Enum);
    enumeration.enumOptions = {"A", "B", "C"};
    FieldInfo stepped = MakeField(FieldKind::Vector3);
    stepped.replicationStep = 0.25;
    FieldInfo ranged = MakeField(FieldKind::Float);
    ranged.range = std::make_pair(-1.0, 1.0);
    const std::vector<FieldInfo> fields = {
        MakeField(FieldKind::Bool),       MakeField(FieldKind::Int),        MakeField(FieldKind::Float),
        ranged,                           MakeField(FieldKind::String),     MakeField(FieldKind::Vector2),
        MakeField(FieldKind::Vector3),    stepped,                          MakeField(FieldKind::Vector4),
        MakeField(FieldKind::Quaternion), MakeField(FieldKind::Color, "Color"),
        MakeField(FieldKind::Color, "Vector3"), enumeration,                MakeField(FieldKind::AssetRef),
        MakeField(FieldKind::GameObjectRef)};
    FakeReferences references;
    references.ids["12345678-9abc-4def-8123-456789abcdef"] = 5;

    N2Net::SimRandom random(2024);
    std::size_t accepted = 0;
    for (int round = 0; round < 4000; ++round)
    {
        std::vector<std::uint8_t> bytes(random.NextBelow(40));
        for (std::uint8_t &byte : bytes)
        {
            byte = static_cast<std::uint8_t>(random.NextBelow(256));
        }
        for (const FieldInfo &field : fields)
        {
            N2Net::ByteReader reader(bytes);
            json value;
            const DecodeStatus status = DecodeField(field, reader, &references, value);
            if (status == DecodeStatus::Ok)
            {
                ++accepted;
                // What decodes encodes again, and to the same value (the codec's own output is a fixed point)
                std::vector<std::uint8_t> again;
                if (field.kind != FieldKind::GameObjectRef)
                {
                    EXPECT_TRUE(EncodeField(field, value, &references, again)) << FieldKindName(field.kind);
                    EXPECT_LE(again.size(), MaxEncodedBytes(field));
                }
                if (field.kind == FieldKind::String)
                {
                    EXPECT_LE(value.get<std::string>().size(), kMaxReplicatedStringBytes);
                }
            }
        }
    }
    EXPECT_GT(accepted, 0u) << "the fuzz input never produced a value: the test checks nothing";
}

TEST(ReplicationCodecTest, TheFormatHashSeesEverythingThatChangesTheBytes)
{
    const auto hashOf = [](const FieldInfo &field)
    {
        Fnv1a64 hash;
        HashFieldFormat(hash, field);
        return hash.Value();
    };
    const FieldInfo base = MakeField(FieldKind::Float);
    EXPECT_EQ(hashOf(base), hashOf(MakeField(FieldKind::Float)));

    FieldInfo renamed = base;
    renamed.name = "other";
    EXPECT_NE(hashOf(base), hashOf(renamed));
    EXPECT_NE(hashOf(base), hashOf(MakeField(FieldKind::Int)));
    FieldInfo stepped = base;
    stepped.replicationStep = 0.1;
    EXPECT_NE(hashOf(base), hashOf(stepped));
    FieldInfo ranged = base;
    ranged.range = std::make_pair(0.0, 1.0);
    EXPECT_NE(hashOf(base), hashOf(ranged));
    FieldInfo ranged2 = base;
    ranged2.range = std::make_pair(0.0, 2.0);
    EXPECT_NE(hashOf(ranged), hashOf(ranged2));
    FieldInfo contained = base;
    contained.container = "scriptData";
    EXPECT_NE(hashOf(base), hashOf(contained));

    FieldInfo enumOne = MakeField(FieldKind::Enum);
    enumOne.enumOptions = {"A", "B"};
    FieldInfo enumTwo = enumOne;
    enumTwo.enumOptions = {"A", "B", "C"};
    EXPECT_NE(hashOf(enumOne), hashOf(enumTwo));

    EXPECT_NE(hashOf(MakeField(FieldKind::Color, "Color")), hashOf(MakeField(FieldKind::Color, "Vector3")));

    // A range does not change the bytes of a field that is not a Float: nothing in the format depends on it, but it is
    // still hashed only where it is used
    FieldInfo intRange = MakeField(FieldKind::Int);
    FieldInfo intRange2 = intRange;
    intRange2.range = std::make_pair(0.0, 5.0);
    EXPECT_EQ(hashOf(intRange), hashOf(intRange2));
}
