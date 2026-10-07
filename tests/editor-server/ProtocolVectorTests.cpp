#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

#include "ProtocolVectors.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// The golden vectors are encoded by generate_test_vectors.py straight from protocol.json; the generated TypeScript
// codecs are checked against the same bytes (editor-server/protocol/tests). The server's hand-written codecs agreeing
// with them is what keeps the C++ server and the TypeScript client in step.

namespace
{
    std::set<std::string> SpecRequestCommands()
    {
        std::ifstream file(N2_PROTOCOL_JSON);
        const json spec = json::parse(file);
        std::set<std::string> commands;
        for (const auto &[name, command] : spec.at("commands").items())
        {
            if (!command.at("request").empty())
                commands.insert(name);
        }
        return commands;
    }

    std::set<std::string> SpecResponses()
    {
        std::ifstream file(N2_PROTOCOL_JSON);
        const json spec = json::parse(file);
        std::set<std::string> responses;
        for (const auto &[name, id] : spec.at("responses").items())
        {
            responses.insert(name);
        }
        return responses;
    }

    std::vector<float> Floats(const BufferWriter &w)
    {
        BufferReader r(w.Data());
        std::vector<float> values;
        while (r.HasData())
        {
            values.push_back(r.ReadF32());
        }
        return values;
    }
}

TEST(ProtocolVectorTest, VectorsAreForThisProtocolVersion)
{
    EXPECT_EQ(ProtocolVectors::Load().at("protocolVersion").get<std::string>(), ProtocolVersion)
        << "test-vectors.json is stale: rerun editor-server/protocol/generators/generate_test_vectors.py";
}

TEST(ProtocolVectorTest, EveryRequestAndResponseHasAVectorAndAServerCodec)
{
    std::set<std::string> requestVectors;
    for (const auto &[name, vector] : ProtocolVectors::LoadRequests())
        requestVectors.insert(name);
    std::set<std::string> decoders;
    for (const auto &[name, decoder] : ProtocolVectors::RequestDecoders())
        decoders.insert(name);
    EXPECT_EQ(requestVectors, SpecRequestCommands());
    EXPECT_EQ(decoders, SpecRequestCommands());

    std::set<std::string> responseVectors;
    for (const auto &[name, vector] : ProtocolVectors::LoadResponses())
        responseVectors.insert(name);
    std::set<std::string> builders;
    for (const auto &[name, builder] : ProtocolVectors::ResponseBuilders())
        builders.insert(name);
    EXPECT_EQ(responseVectors, SpecResponses());
    EXPECT_EQ(builders, SpecResponses());
}

// Each request vector's payload, read by the server's deserializer, gives the vector's field values, and the
// deserializer reads all of it
TEST(ProtocolVectorTest, ServerDeserializersReadTheVectorRequests)
{
    const auto vectors = ProtocolVectors::LoadRequests();
    ASSERT_FALSE(vectors.empty());
    for (const auto &[name, vector] : vectors)
    {
        const auto decoder = ProtocolVectors::RequestDecoders().find(name);
        ASSERT_NE(decoder, ProtocolVectors::RequestDecoders().end()) << name;

        const std::vector<uint8_t> payload = ProtocolVectors::FromHex(vector.at("payload").get<std::string>());
        BufferReader reader(payload);
        json decoded;
        ASSERT_NO_THROW(decoded = decoder->second(reader)) << name;
        EXPECT_EQ(decoded, vector.at("fields")) << name << ": the server read " << decoded.dump();
        EXPECT_EQ(reader.Remaining(), 0u) << name << ": the server left bytes unread";
    }
}

// Each response vector's fields, written by the server's builder, give the vector's frame: byte for byte, except that
// json fields are compared as parsed values (how JSON text is written, its key order or spacing, is free)
TEST(ProtocolVectorTest, ServerBuildersWriteTheVectorResponses)
{
    std::ifstream file(N2_PROTOCOL_JSON);
    const json spec = json::parse(file);
    const auto vectors = ProtocolVectors::LoadResponses();
    ASSERT_FALSE(vectors.empty());
    for (const auto &[name, vector] : vectors)
    {
        const std::vector<uint8_t> payload = ProtocolVectors::FromHex(vector.at("payload").get<std::string>());
        const uint8_t id = static_cast<uint8_t>(std::stoi(vector.at("id").get<std::string>(), nullptr, 16));
        const std::vector<uint8_t> frame = ProtocolVectors::BuildResponse(name, vector);
        ASSERT_GE(frame.size(), 5u) << name;

        json fields = json::object();
        for (const auto &[command, declaration] : spec.at("commands").items())
        {
            if (declaration.at("response").at("type") == name && declaration.at("response").contains("fields"))
                fields = declaration.at("response").at("fields");
        }

        if (!ProtocolVectors::HasJsonField(fields))
        {
            BufferWriter expected;
            expected.WriteU8(id);
            expected.WriteU32(static_cast<uint32_t>(payload.size()));
            expected.WriteBytes(payload);
            EXPECT_EQ(ProtocolVectors::ToHex(frame), ProtocolVectors::ToHex(expected.Release())) << name;
            continue;
        }

        EXPECT_EQ(frame[0], id) << name;
        const std::span<const uint8_t> written = std::span<const uint8_t>(frame).subspan(5);
        json writtenValues;
        ASSERT_NO_THROW(writtenValues = ProtocolVectors::DecodeBySpec(spec, fields, written)) << name;
        EXPECT_EQ(writtenValues, ProtocolVectors::DecodeBySpec(spec, fields, payload)) << name;
    }
}

// ==================== quat, mat4 and json ====================
// No command uses quat or mat4 yet, so their codecs are pinned here (and by the TypeScript fixture tests)

TEST(ProtocolFieldCodecTest, QuatIsXyzwWithWLast)
{
    BufferWriter w;
    WriteQuat(w, Math::Quaternion(4.0f, 1.0f, 2.0f, 3.0f)); // w, x, y, z
    EXPECT_EQ(Floats(w), (std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f}));

    BufferReader r(w.Data());
    const Math::Quaternion q = ReadQuat(r);
    EXPECT_EQ(q.GetX(), 1.0f);
    EXPECT_EQ(q.GetY(), 2.0f);
    EXPECT_EQ(q.GetZ(), 3.0f);
    EXPECT_EQ(q.GetW(), 4.0f);
    EXPECT_FALSE(r.HasData());
}

TEST(ProtocolFieldCodecTest, Mat4IsColumnMajorWithTheTranslationInElements12To14)
{
    const auto translation = Math::Matrix<float, 4, 4>::Translation(Math::Vector3(5.0f, 6.0f, 7.0f));
    BufferWriter w;
    WriteMat4(w, translation);
    const std::vector<float> elements = Floats(w);
    ASSERT_EQ(elements.size(), 16u);
    EXPECT_EQ(elements[12], 5.0f);
    EXPECT_EQ(elements[13], 6.0f);
    EXPECT_EQ(elements[14], 7.0f);
    EXPECT_EQ(elements[15], 1.0f);
    EXPECT_EQ(elements[3], 0.0f) << "row 3 of column 0: a row-major write would put the translation here";

    // Element col * 4 + row is m(row, col), whatever the matrix
    Math::Matrix<float, 4, 4> m;
    for (size_t row = 0; row < 4; ++row)
        for (size_t col = 0; col < 4; ++col)
            m(row, col) = static_cast<float>(row * 10 + col);
    BufferWriter all;
    WriteMat4(all, m);
    const std::vector<float> written = Floats(all);
    for (size_t row = 0; row < 4; ++row)
        for (size_t col = 0; col < 4; ++col)
            EXPECT_EQ(written[col * 4 + row], m(row, col)) << row << ", " << col;

    BufferReader r(all.Data());
    EXPECT_EQ(ReadMat4(r), m);
    EXPECT_FALSE(r.HasData());
}

TEST(ProtocolFieldCodecTest, JsonIsAStringOfCompactJsonWithSortedKeys)
{
    BufferWriter w;
    WriteJson(w, json{{"b", 1}, {"a", {true, nullptr, "x"}}});
    BufferReader r(w.Data());
    EXPECT_EQ(r.ReadString(), R"({"a":[true,null,"x"],"b":1})");

    BufferReader again(w.Data());
    EXPECT_EQ(ReadJson(again), (json{{"a", {true, nullptr, "x"}}, {"b", 1}}));
}

TEST(ProtocolFieldCodecTest, JsonWritesInvalidUtf8AsAReplacementCharacter)
{
    BufferWriter w;
    ASSERT_NO_THROW(WriteJson(w, json(std::string("bad \xFF byte"))));
    BufferReader r(w.Data());
    EXPECT_EQ(r.ReadString(), "\"bad \xEF\xBF\xBD byte\"");
}

TEST(ProtocolFieldCodecTest, ATrailingFieldAnOlderClientDoesntSendGetsItsDefault)
{
    // A request as a newer client sends it (with the trailing bool) and as an older one does (without)
    BufferWriter newer;
    newer.WriteString("id");
    newer.WriteBool(false);
    BufferWriter older;
    older.WriteString("id");

    BufferReader newerReader(newer.Data());
    (void)newerReader.ReadString();
    EXPECT_FALSE(ReadTrailing(newerReader, &BufferReader::ReadBool, true));
    EXPECT_FALSE(newerReader.HasData());

    BufferReader olderReader(older.Data());
    (void)olderReader.ReadString();
    EXPECT_TRUE(ReadTrailing(olderReader, &BufferReader::ReadBool, true));
    EXPECT_EQ(ReadTrailing(olderReader, [](BufferReader &r) { return r.ReadString(); }, std::string{"fallback"}),
              "fallback");
}

TEST(ProtocolFieldCodecTest, ReadingTextThatIsntJsonThrows)
{
    BufferWriter w;
    w.WriteString("{not json");
    BufferReader r(w.Data());
    EXPECT_THROW((void)ReadJson(r), nlohmann::json::parse_error);
}
