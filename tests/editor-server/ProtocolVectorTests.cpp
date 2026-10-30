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

// A peer from before a message's optional trailing fields (protocol.json's optionalTrailing; the vectors' shortPayloads)
// sends a payload that ends before them. The server's Hello deserializer reads such a request with the defaults
// (a 1.10 client sends no errorCodes and no user, a 1.11 one no user); for a response, the server's builder writes the
// short payload followed by exactly the omitted fields (a 1.11 host's ServerInfo has no user or role, a 1.10 host's no
// readOnly either), which is what a client's lenient decoder relies on.
TEST(ProtocolVectorTest, ShortPayloadsOfOlderPeersDecodeWithTheDefaults)
{
    const json vectors = ProtocolVectors::Load().value("shortPayloads", json::array());
    ASSERT_FALSE(vectors.empty());
    bool sawRequest = false;
    bool sawResponse = false;
    for (const json &vector : vectors)
    {
        const std::vector<uint8_t> payload = ProtocolVectors::FromHex(vector.at("payload").get<std::string>());
        if (vector.contains("request"))
        {
            sawRequest = true;
            const std::string name = vector.at("request").get<std::string>();
            const auto decoder = ProtocolVectors::RequestDecoders().find(name);
            ASSERT_NE(decoder, ProtocolVectors::RequestDecoders().end()) << name;
            BufferReader reader(payload);
            json decoded;
            ASSERT_NO_THROW(decoded = decoder->second(reader)) << name;
            EXPECT_EQ(decoded, vector.at("fields")) << name << ": the server read " << decoded.dump();
            EXPECT_EQ(reader.Remaining(), 0u) << name;
        }
        else
        {
            sawResponse = true;
            const std::string name = vector.at("response").get<std::string>();
            // The vector's fields hold the defaults of the omitted fields: the builder writes the short payload, then
            // those fields at their defaults (a bool is one zero byte, a string a zero length)
            std::vector<uint8_t> expected = payload;
            ASSERT_TRUE(vector.contains("omitted") && !vector.at("omitted").empty()) << name;
            for (const json &field : vector.at("omitted"))
            {
                const json &value = vector.at("fields").at(field.get<std::string>());
                ASSERT_TRUE(value.is_boolean() || value.is_string()) << name << "." << field;
                EXPECT_TRUE(value.is_boolean() ? !value.get<bool>() : value.get<std::string>().empty())
                    << name << "." << field << ": a default";
                expected.insert(expected.end(), value.is_boolean() ? 1u : 4u, uint8_t{0});
            }
            const std::vector<uint8_t> frame = ProtocolVectors::BuildResponse(name, vector);
            ASSERT_EQ(frame.size(), 5u + expected.size()) << name;
            const std::vector<uint8_t> built(frame.begin() + 5, frame.end());
            EXPECT_EQ(ProtocolVectors::ToHex(built), ProtocolVectors::ToHex(expected)) << name;
        }
    }
    EXPECT_TRUE(sawRequest);
    EXPECT_TRUE(sawResponse);

    // Hello as an old (1.10) client sends it: no errorCodes, so the host sends plain Errors, and no user
    BufferWriter oldHello;
    oldHello.WriteString("editor");
    oldHello.WriteString("1.10.0");
    oldHello.WriteString("");
    BufferReader reader(oldHello.Data());
    const HelloCmd cmd = HelloCmd::Deserialize(reader);
    EXPECT_FALSE(cmd.errorCodes);
    EXPECT_EQ(cmd.user, "");
    EXPECT_FALSE(reader.HasData());

    // A 1.11 client sends errorCodes but no user: the user is empty, which a tokens-file host reads as "match the token
    // across every entry"
    BufferWriter helloWithCodes;
    helloWithCodes.WriteString("editor");
    helloWithCodes.WriteString("1.11.0");
    helloWithCodes.WriteString("token");
    helloWithCodes.WriteBool(true);
    BufferReader codesReader(helloWithCodes.Data());
    const HelloCmd withCodes = HelloCmd::Deserialize(codesReader);
    EXPECT_TRUE(withCodes.errorCodes);
    EXPECT_EQ(withCodes.user, "");
    EXPECT_FALSE(codesReader.HasData());

    // A 1.12 client names its user
    BufferWriter helloWithUser;
    helloWithUser.WriteString("editor");
    helloWithUser.WriteString("1.12.0");
    helloWithUser.WriteString("token");
    helloWithUser.WriteBool(false);
    helloWithUser.WriteString("alice");
    BufferReader userReader(helloWithUser.Data());
    const HelloCmd withUser = HelloCmd::Deserialize(userReader);
    EXPECT_FALSE(withUser.errorCodes);
    EXPECT_EQ(withUser.user, "alice");
    EXPECT_FALSE(withUser.yield) << "a 1.12 client does not send yield: false";
    EXPECT_FALSE(userReader.HasData());

    // A 1.22 client says whether it yields to people (after errorCodes and user)
    BufferWriter helloWithYield;
    helloWithYield.WriteString("agent");
    helloWithYield.WriteString("1.22.0");
    helloWithYield.WriteString("token");
    helloWithYield.WriteBool(true);
    helloWithYield.WriteString("agent-claude");
    helloWithYield.WriteBool(true);
    BufferReader yieldReader(helloWithYield.Data());
    const HelloCmd withYield = HelloCmd::Deserialize(yieldReader);
    EXPECT_TRUE(withYield.errorCodes);
    EXPECT_EQ(withYield.user, "agent-claude");
    EXPECT_TRUE(withYield.yield);
    EXPECT_FALSE(yieldReader.HasData());
}

// A 1.23 client's SetNetworkSettings with the newly accepted authoritative.timeDilation object: the frame layout did not
// change (preset string, settings json), so this pins the bytes such a request is and what the server reads from them
TEST(ProtocolVectorTest, ASetNetworkSettingsWithTimeDilationReadsAsItsMergePatch)
{
    const json settings = {{"authoritative", {{"timeDilation", {{"enabled", true}, {"minScale", 0.9}, {"maxScale", 1.1}}}}}};
    const std::string expectedHex =
        "00000000510000007b22617574686f7269746174697665223a7b2274696d6544696c6174696f6e223a7b22656e61626c6564223a7472"
        "75652c226d61785363616c65223a312e312c226d696e5363616c65223a302e397d7d7d";

    BufferWriter w;
    w.WriteString("");
    WriteJson(w, settings);
    EXPECT_EQ(ProtocolVectors::ToHex(w.Release()), expectedHex);

    const std::vector<uint8_t> payload = ProtocolVectors::FromHex(expectedHex);
    BufferReader reader(payload);
    const SetNetworkSettingsCmd cmd = SetNetworkSettingsCmd::Deserialize(reader);
    EXPECT_EQ(cmd.preset, "");
    EXPECT_EQ(cmd.settings, settings);
    EXPECT_FALSE(reader.HasData());
}

// Each response vector's fields, written by the server's builder, give the vector's frame: byte for byte, except that
// json fields are compared as parsed values (how JSON text is written, its key order or spacing, is free)
TEST(ProtocolVectorTest, ServerBuildersWriteTheVectorResponses)
{
    std::ifstream file(N2_PROTOCOL_JSON);
    // ordered: fields go on the wire in the order protocol.json lists them (json would sort the keys)
    const nlohmann::ordered_json spec = nlohmann::ordered_json::parse(file);
    const auto vectors = ProtocolVectors::LoadResponses();
    ASSERT_FALSE(vectors.empty());
    for (const auto &[name, vector] : vectors)
    {
        const std::vector<uint8_t> payload = ProtocolVectors::FromHex(vector.at("payload").get<std::string>());
        const uint8_t id = static_cast<uint8_t>(std::stoi(vector.at("id").get<std::string>(), nullptr, 16));
        const std::vector<uint8_t> frame = ProtocolVectors::BuildResponse(name, vector);
        ASSERT_GE(frame.size(), 5u) << name;

        nlohmann::ordered_json fields = nlohmann::ordered_json::object();
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

// A response with a second shape has a vector of its own (FrameUpdate that isn't modified: 13 bytes, no pixels)
TEST(ProtocolVectorTest, ServerBuildersWriteTheResponseVariants)
{
    const json variants = ProtocolVectors::Load().value("responseVariants", json::array());
    for (const json &variant : variants)
    {
        const std::string name = variant.at("response").get<std::string>();
        const std::vector<uint8_t> payload = ProtocolVectors::FromHex(variant.at("payload").get<std::string>());
        BufferWriter expected;
        expected.WriteU8(static_cast<uint8_t>(std::stoi(variant.at("id").get<std::string>(), nullptr, 16)));
        expected.WriteU32(static_cast<uint32_t>(payload.size()));
        expected.WriteBytes(payload);
        EXPECT_EQ(ProtocolVectors::ToHex(ProtocolVectors::BuildResponse(name, variant)),
                  ProtocolVectors::ToHex(expected.Release()))
            << name;
        if (name == "FrameUpdate")
        {
            EXPECT_EQ(payload.size(), 13u) << "revision, modified, width, height: no pixels";
            EXPECT_EQ(variant.at("fields").at("modified"), false);
        }
    }
    EXPECT_FALSE(variants.empty());
}

// ==================== quat, mat4 and json ====================
// The element-level layout of quat and mat4 (SetLocalTransform and GetEntity use them, and their vectors cover
// the commands), pinned here and by the TypeScript fixture tests

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
