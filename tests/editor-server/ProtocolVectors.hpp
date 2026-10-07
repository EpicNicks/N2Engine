#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Serialization.hpp>

// The golden vectors (editor-server/protocol/test-vectors.json, written by generate_test_vectors.py from
// protocol.json) and the server's hand-written codecs, keyed by message name, for ProtocolVectorTests and
// ProtocolSpecTests. Each vector is {"fields": {...}, "payload": "<hex>"}, plus "command" or "response" and "id".
namespace ProtocolVectors
{
    /// The whole test-vectors.json
    nlohmann::json Load();
    /// Command name -> its request vector
    std::map<std::string, nlohmann::json> LoadRequests();
    /// Response name -> its vector
    std::map<std::string, nlohmann::json> LoadResponses();

    std::vector<uint8_t> FromHex(const std::string &hex);
    std::string ToHex(const std::vector<uint8_t> &bytes);

    /// Reads a request payload with the server's deserializer and returns its fields as the vectors write them
    using RequestDecoder = std::function<nlohmann::json(N2Engine::Editor::Protocol::BufferReader &)>;
    /// One per command with request fields
    const std::map<std::string, RequestDecoder> &RequestDecoders();

    /// Builds a whole response frame with the server's builder from a vector's fields
    using ResponseBuilder = std::function<void(N2Engine::Editor::Protocol::BufferWriter &, const nlohmann::json &)>;
    /// One per response
    const std::map<std::string, ResponseBuilder> &ResponseBuilders();

    /// The frame the server's builder writes for a response vector (empty, after a test failure, without a builder)
    std::vector<uint8_t> BuildResponse(const std::string &response, const nlohmann::json &vector);

    /// A payload's fields as values, read by protocol.json's field list (fields: name -> type): json fields parsed,
    /// bytes as hex, mat4 as 16 numbers, structs as objects. Two payloads that differ only in how their JSON text
    /// is written (key order, spacing) decode to equal values. Throws std::out_of_range on a short payload.
    nlohmann::json DecodeBySpec(const nlohmann::json &spec, const nlohmann::json &fields,
                                std::span<const uint8_t> payload);
    /// Whether any of the fields is json
    bool HasJsonField(const nlohmann::json &fields);
}
