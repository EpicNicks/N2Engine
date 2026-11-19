#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <renderer/common/UserShader.hpp>

// Loads a cooked user shader of tests/renderer/fixtures/user-shaders/<name>/ (manifest.json and shader.vulkan.frag.spv, made by
// tools/n2shader through scripts/cook-user-shader-fixtures.sh) as the UserShaderBlob a backend is given. This is the
// manifest-to-blob mapping the engine's loader (proposal stage 5) will have; the tests do it here so the backends can be
// tested before that exists. Every backend's user-shader tests share it.
namespace UserShaderTesting
{
    using Renderer::Common::UserParam;
    using Renderer::Common::UserParamType;
    using Renderer::Common::UserShaderBlob;
    using Renderer::Common::UserTextureDefault;

    inline std::string FixtureDirectory(const std::string& name)
    {
        return std::string(N2_RENDERER_TEST_DIR) + "/fixtures/user-shaders/" + name + "/";
    }

    inline std::vector<std::uint8_t> ReadBytes(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("cannot open " + path);
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    inline UserParamType ParamTypeOf(const std::string& text)
    {
        if (text == "float") return UserParamType::Float;
        if (text == "int") return UserParamType::Int;
        if (text == "bool") return UserParamType::Bool;
        if (text == "vec2") return UserParamType::Vec2;
        if (text == "vec3") return UserParamType::Vec3;
        if (text == "vec4") return UserParamType::Vec4;
        if (text == "color") return UserParamType::Color;
        if (text == "texture") return UserParamType::Texture;
        throw std::runtime_error("unknown parameter type " + text);
    }

    inline UserTextureDefault TextureDefaultOf(const std::string& text)
    {
        if (text == "white") return UserTextureDefault::White;
        if (text == "black") return UserTextureDefault::Black;
        if (text == "normal") return UserTextureDefault::Normal;
        if (text == "none") return UserTextureDefault::None;
        throw std::runtime_error("unknown texture default " + text);
    }

    /// `blob` with its fragment entry point's interface cut down to the varyings in `keepMask` (the kUserVarying bits): the
    /// inputs of the other locations stay declared but are no longer listed, which is what a module whose unread inputs were
    /// stripped (by an optimiser, or a compiler that lists only what is used) looks like. glslang lists every declared input,
    /// so the cooked fixtures always read as "all five"; this is how the narrower vertex variants are reached
    inline UserShaderBlob WithFragmentInputsOnly(UserShaderBlob blob, const std::uint32_t keepMask)
    {
        constexpr std::uint32_t OpEntryPoint = 15, OpVariable = 59, OpDecorate = 71, DecorationLocation = 30, StorageInput = 1;
        std::vector<std::uint32_t>& words = blob.fragmentSpirv;
        std::map<std::uint32_t, std::uint32_t> location;
        std::set<std::uint32_t> inputs;
        std::size_t entryAt = 0;
        for (std::size_t at = 5; at < words.size(); at += words[at] >> 16)
        {
            const std::uint32_t opcode = words[at] & 0xFFFFu;
            if (opcode == OpDecorate && words[at + 2] == DecorationLocation)
            {
                location[words[at + 1]] = words[at + 3];
            }
            else if (opcode == OpVariable && words[at + 3] == StorageInput)
            {
                inputs.insert(words[at + 2]);
            }
            else if (opcode == OpEntryPoint)
            {
                entryAt = at;
            }
        }
        const std::size_t count = words[entryAt] >> 16;
        std::size_t name = entryAt + 3;
        while (name < entryAt + count)
        {
            const std::uint32_t word = words[name++];
            if ((word & 0xFFu) == 0 || (word & 0xFF00u) == 0 || (word & 0xFF0000u) == 0 || (word & 0xFF000000u) == 0)
            {
                break;
            }
        }
        std::vector<std::uint32_t> kept;
        for (std::size_t i = name; i < entryAt + count; ++i)
        {
            const std::uint32_t id = words[i];
            const bool drop = inputs.count(id) != 0 && location.count(id) != 0 && ((keepMask >> location[id]) & 1u) == 0;
            if (!drop)
            {
                kept.push_back(id);
            }
        }
        words.erase(words.begin() + static_cast<std::ptrdiff_t>(name), words.begin() + static_cast<std::ptrdiff_t>(entryAt + count));
        words.insert(words.begin() + static_cast<std::ptrdiff_t>(name), kept.begin(), kept.end());
        words[entryAt] = static_cast<std::uint32_t>(((name - entryAt) + kept.size()) << 16) | OpEntryPoint;
        return blob;
    }

    /// The blob of a fixture; throws (a test failure) when a file is missing or malformed
    inline UserShaderBlob LoadFixture(const std::string& name)
    {
        const std::string directory = FixtureDirectory(name);
        const std::vector<std::uint8_t> manifestBytes = ReadBytes(directory + "manifest.json");
        const nlohmann::json manifest = nlohmann::json::parse(manifestBytes.begin(), manifestBytes.end());

        UserShaderBlob blob;
        blob.name = manifest.at("name").get<std::string>();
        blob.sourcePath = manifest.at("source").at("file").get<std::string>();
        blob.contract = manifest.at("contract").get<std::uint32_t>();
        blob.blockSize = manifest.at("blockSize").get<std::uint32_t>();
        for (const nlohmann::json& use : manifest.at("uses"))
        {
            blob.usesLighting = blob.usesLighting || use.get<std::string>() == "lighting";
        }
        for (const nlohmann::json& entry : manifest.at("params"))
        {
            UserParam param;
            param.name = entry.at("name").get<std::string>();
            param.type = ParamTypeOf(entry.at("type").get<std::string>());
            if (param.type == UserParamType::Texture)
            {
                param.slot = entry.at("slot").get<std::uint32_t>();
                param.textureDefault = TextureDefaultOf(entry.at("default").get<std::string>());
            }
            else
            {
                param.offset = entry.at("offset").get<std::uint32_t>();
                param.size = entry.at("size").get<std::uint32_t>();
                const nlohmann::json& value = entry.at("default");
                if (value.is_array())
                {
                    for (std::size_t i = 0; i < value.size() && i < param.defaultValue.size(); ++i)
                    {
                        param.defaultValue[i] = value[i].get<float>();
                    }
                }
                else if (value.is_boolean())
                {
                    param.defaultValue[0] = value.get<bool>() ? 1.0f : 0.0f;
                }
                else
                {
                    param.defaultValue[0] = value.get<float>();
                }
            }
            blob.params.push_back(param);
        }

        const std::vector<std::uint8_t> spirv = ReadBytes(directory + manifest.at("payloads").at("vulkan").at("file").get<std::string>());
        if (spirv.size() % 4 != 0)
        {
            throw std::runtime_error(name + ": the SPIR-V is not a whole number of words");
        }
        blob.fragmentSpirv.resize(spirv.size() / 4);
        for (std::size_t i = 0; i < blob.fragmentSpirv.size(); ++i)
        {
            blob.fragmentSpirv[i] = static_cast<std::uint32_t>(spirv[4 * i]) | (static_cast<std::uint32_t>(spirv[4 * i + 1]) << 8) |
                (static_cast<std::uint32_t>(spirv[4 * i + 2]) << 16) | (static_cast<std::uint32_t>(spirv[4 * i + 3]) << 24);
        }
        return blob;
    }
}
