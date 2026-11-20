#include "engine/rendering/Shader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/Resources.hpp"
#include "engine/io/Vfs.hpp"
#include "engine/rendering/RenderSettings.hpp"
#if !defined(__EMSCRIPTEN__)
#include "engine/rendering/UserShaderCompiler.hpp"
#endif

namespace N2Engine::Rendering
{
    namespace
    {
        // Lives here because anything that uses Shader links this file; RegisterLoader is public too and ResourceLoader::Initialize
        // calls it, for programs that never name Shader
        struct ShaderLoaderRegistrar
        {
            ShaderLoaderRegistrar()
            {
                Shader::RegisterLoader();
            }
        } g_shaderLoaderRegistrar;

        constexpr std::size_t kMaxParams = Renderer::Common::kUserShaderMaxParams;
        constexpr int kTextureSlots = static_cast<int>(Renderer::Common::kUserShaderTextureSlots);

        std::string Trimmed(std::string_view text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            {
                ++begin;
            }
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            {
                --end;
            }
            return std::string(text.substr(begin, end - begin));
        }

        std::optional<ShaderParamType> ParamTypeOf(const std::string &name)
        {
            if (name == "float") return ShaderParamType::Float;
            if (name == "int") return ShaderParamType::Int;
            if (name == "bool") return ShaderParamType::Bool;
            if (name == "vec2") return ShaderParamType::Vec2;
            if (name == "vec3") return ShaderParamType::Vec3;
            if (name == "vec4") return ShaderParamType::Vec4;
            if (name == "color") return ShaderParamType::Color;
            if (name == "texture") return ShaderParamType::Texture;
            if (name == "mat3") return ShaderParamType::Mat3;
            if (name == "mat4") return ShaderParamType::Mat4;
            return std::nullopt;
        }

        const char *ParamTypeName(const ShaderParamType type)
        {
            switch (type)
            {
            case ShaderParamType::Float: return "float";
            case ShaderParamType::Int: return "int";
            case ShaderParamType::Bool: return "bool";
            case ShaderParamType::Vec2: return "vec2";
            case ShaderParamType::Vec3: return "vec3";
            case ShaderParamType::Vec4: return "vec4";
            case ShaderParamType::Color: return "color";
            case ShaderParamType::Texture: return "texture";
            case ShaderParamType::Mat3: return "mat3";
            case ShaderParamType::Mat4: return "mat4";
            }
            return "float";
        }

        std::size_t ComponentCount(const ShaderParamType type)
        {
            switch (type)
            {
            case ShaderParamType::Vec2: return 2;
            case ShaderParamType::Vec3: return 3;
            case ShaderParamType::Vec4:
            case ShaderParamType::Color: return 4;
            default: return 1;
            }
        }

        bool IsIdentifier(const std::string &name)
        {
            if (name.empty() || name.size() > Renderer::Common::kUserShaderMaxNameBytes ||
                !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
            {
                return false;
            }
            return std::ranges::all_of(name, [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        }

        /// The default value of a parameter as the header gives it; false, with `why`, when it does not fit the type
        bool ReadDefault(const nlohmann::json &value, ShaderParam &param, std::string &why)
        {
            const auto number = [&](const nlohmann::json &item) { return item.is_number() && std::isfinite(item.get<double>()); };
            switch (param.type)
            {
            case ShaderParamType::Texture:
                if (!value.is_string() || (value.get<std::string>() != "white" && value.get<std::string>() != "black" &&
                                           value.get<std::string>() != "normal" && value.get<std::string>() != "none"))
                {
                    why = "a texture default is \"white\", \"black\", \"normal\" or \"none\"";
                    return false;
                }
                param.defaultTexture = value.get<std::string>();
                return true;
            case ShaderParamType::Bool:
                if (!value.is_boolean())
                {
                    why = "a bool default is true or false";
                    return false;
                }
                param.defaultValue[0] = value.get<bool>() ? 1.0f : 0.0f;
                return true;
            case ShaderParamType::Float:
            case ShaderParamType::Int:
                if (!number(value))
                {
                    why = "a number default is expected";
                    return false;
                }
                param.defaultValue[0] = param.type == ShaderParamType::Int ? static_cast<float>(std::trunc(value.get<double>()))
                                                                            : static_cast<float>(value.get<double>());
                return true;
            case ShaderParamType::Mat3:
            case ShaderParamType::Mat4:
                why = std::format("a {} starts as the identity matrix and takes no default", ParamTypeName(param.type));
                return false;
            default:
            {
                const std::size_t count = ComponentCount(param.type);
                if (!value.is_array() || value.size() != count || !std::ranges::all_of(value, number))
                {
                    why = std::format("a {} default is an array of {} numbers", ParamTypeName(param.type), count);
                    return false;
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    param.defaultValue[i] = value[i].get<float>();
                }
                return true;
            }
            }
        }
    }

    // ===== parsing =====

    std::shared_ptr<Shader> Shader::FromSource(const std::string_view text, const std::string_view debugName)
    {
        auto shader = std::make_shared<Shader>();
        if (!shader->Parse(text, debugName))
        {
            return nullptr;
        }
        return shader;
    }

    bool Shader::Parse(const std::string_view text, const std::string_view debugName)
    {
        const auto fail = [&](const std::string &message, const int line = 0)
        {
            Logger::Error(line > 0 ? std::format("Shader {}:{}: {}", debugName, line, message)
                                   : std::format("Shader {}: {}", debugName, message));
            return false;
        };
        if (text.size() > Renderer::Common::kUserShaderMaxGlslBytes)
        {
            return fail(std::format("the file is {} bytes; the limit is {}", text.size(), Renderer::Common::kUserShaderMaxGlslBytes));
        }

        // The header is everything before the first line that is exactly "#stage fragment"
        std::string header;
        bool foundStage = false;
        int line = 0;
        std::size_t at = 0;
        while (at <= text.size() && !foundStage)
        {
            const std::size_t end = text.find('\n', at);
            const std::string_view lineText = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
            ++line;
            if (Trimmed(lineText) == "#stage fragment")
            {
                foundStage = true;
                break;
            }
            header.append(lineText);
            header.push_back('\n');
            if (end == std::string_view::npos)
            {
                break;
            }
            at = end + 1;
        }
        if (!foundStage)
        {
            return fail("no `#stage fragment` line: a .n2shader is a JSON header, then that line, then the GLSL");
        }

        nlohmann::json json;
        try
        {
            json = nlohmann::json::parse(header);
        }
        catch (const std::exception &e)
        {
            return fail(std::format("the header is not valid JSON: {}", e.what()));
        }
        if (!json.is_object())
        {
            return fail("the header must be a JSON object");
        }
        const auto format = json.find("n2shader");
        if (format == json.end() || !format->is_number_integer())
        {
            return fail("the header has no \"n2shader\" format number");
        }
        if (format->get<int>() != 1)
        {
            return fail(std::format("format {} is not one this engine reads (it reads format 1)", format->get<int>()));
        }

        std::string name = "shader";
        if (const auto it = json.find("name"); it != json.end())
        {
            if (!it->is_string())
            {
                return fail("\"name\" must be a string");
            }
            name = it->get<std::string>();
        }

        std::vector<ShaderParam> params;
        std::set<std::string> seen;
        if (const auto it = json.find("params"); it != json.end())
        {
            if (!it->is_array())
            {
                return fail("\"params\" must be an array");
            }
            if (it->size() > kMaxParams)
            {
                return fail(std::format("{} parameters; the limit is {}", it->size(), kMaxParams));
            }
            for (const nlohmann::json &entry : *it)
            {
                if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !entry.contains("type") ||
                    !entry["type"].is_string())
                {
                    return fail("each parameter is an object with a \"name\" and a \"type\"");
                }
                ShaderParam param;
                param.name = entry["name"].get<std::string>();
                if (!IsIdentifier(param.name) || param.name.rfind("n2_", 0) == 0 || param.name.rfind("gl_", 0) == 0)
                {
                    return fail(std::format("parameter name '{}' is not an identifier (at most {} characters, not starting with n2_ or gl_)",
                                            param.name, Renderer::Common::kUserShaderMaxNameBytes));
                }
                if (!seen.insert(param.name).second)
                {
                    return fail(std::format("parameter '{}' is declared twice", param.name));
                }
                const auto type = ParamTypeOf(entry["type"].get<std::string>());
                if (!type)
                {
                    return fail(std::format("parameter '{}': unknown type '{}'", param.name, entry["type"].get<std::string>()));
                }
                param.type = *type;
                if (param.type == ShaderParamType::Color)
                {
                    param.defaultValue = {1.0f, 1.0f, 1.0f, 1.0f};
                }
                if (entry.contains("default"))
                {
                    std::string why;
                    if (!ReadDefault(entry["default"], param, why))
                    {
                        return fail(std::format("parameter '{}': {}", param.name, why));
                    }
                }
                if (entry.contains("label") && entry["label"].is_string())
                {
                    param.label = entry["label"].get<std::string>();
                }
                if (entry.contains("min") && entry["min"].is_number())
                {
                    param.min = entry["min"].get<float>();
                }
                if (entry.contains("max") && entry["max"].is_number())
                {
                    param.max = entry["max"].get<float>();
                }
                params.push_back(std::move(param));
            }
        }

        // Texture slots as the cook assigns them: the reserved baseColorTexture takes slot 0 (the material's base texture), the others
        // follow in declaration order
        int slot = 0;
        for (ShaderParam &param : params)
        {
            if (param.type == ShaderParamType::Texture && param.name == "baseColorTexture")
            {
                param.slot = slot++;
            }
        }
        for (ShaderParam &param : params)
        {
            if (param.type == ShaderParamType::Texture && param.name != "baseColorTexture")
            {
                param.slot = slot++;
            }
        }
        if (slot > kTextureSlots)
        {
            return fail(std::format("{} texture parameters; the limit is {}", slot, kTextureSlots));
        }

        bool usesLighting = false;
        if (const auto it = json.find("uses"); it != json.end())
        {
            if (!it->is_array())
            {
                return fail("\"uses\" must be an array");
            }
            for (const nlohmann::json &use : *it)
            {
                if (!use.is_string() || use.get<std::string>() != "lighting")
                {
                    return fail(std::format("\"uses\" may only list \"lighting\" (found {})", use.dump()));
                }
                usesLighting = true;
            }
        }

        ShaderStateHints state;
        if (const auto it = json.find("state"); it != json.end())
        {
            if (!it->is_object())
            {
                return fail("\"state\" must be an object");
            }
            if (it->contains("blend") && (*it)["blend"].is_string())
            {
                state.blend = (*it)["blend"].get<std::string>();
            }
            if (it->contains("cull") && (*it)["cull"].is_string())
            {
                state.cull = (*it)["cull"].get<std::string>();
            }
            if (it->contains("depthWrite") && (*it)["depthWrite"].is_boolean())
            {
                state.depthWrite = (*it)["depthWrite"].get<bool>();
            }
            if (it->contains("queue") && (*it)["queue"].is_string())
            {
                state.queue = (*it)["queue"].get<std::string>();
            }
        }

        _name = std::move(name);
        _params = std::move(params);
        _usesLighting = usesLighting;
        _state = std::move(state);
        // The text the runtime compile and the cache key read: what the cook's parser sees (no byte order mark, LF line ends)
        _text.clear();
        _text.reserve(text.size());
        std::string_view rest = text;
        if (rest.starts_with("\xEF\xBB\xBF"))
        {
            rest.remove_prefix(3);
        }
        for (std::size_t i = 0; i < rest.size(); ++i)
        {
            if (rest[i] == '\r' && (i + 1 == rest.size() || rest[i + 1] == '\n'))
            {
                continue;
            }
            _text.push_back(rest[i]);
        }
        // A custom vertex stage: a line that is exactly `#stage vertex` after the fragment stage's line
        _hasVertexStage = false;
        {
            const std::size_t fragmentAt = _text.find("\n#stage fragment\n");
            _hasVertexStage = fragmentAt != std::string::npos && _text.find("\n#stage vertex\n", fragmentAt) != std::string::npos;
            if (!_hasVertexStage && fragmentAt != std::string::npos && _text.size() >= 14 &&
                _text.compare(_text.size() - 14, 14, "\n#stage vertex") == 0)
            {
                _hasVertexStage = true;
            }
        }
        return true;
    }

    bool Shader::GetStage(std::string &source, int &firstLine) const
    {
        std::size_t begin = 0;
        int line = 1;
        while (begin <= _text.size())
        {
            const std::size_t end = _text.find('\n', begin);
            const std::string_view lineText = std::string_view(_text).substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            if (lineText == "#stage fragment")
            {
                firstLine = line + 1;
                source = end == std::string::npos ? std::string() : _text.substr(end + 1);
                // The GLSL ends where an authored WebGPU module begins (stage 9f): that block is the WebGPU payload's, never compiled here
                constexpr std::string_view wgslStage = "#stage wgsl";
                constexpr std::string_view vertexStage = "#stage vertex";
                for (std::size_t at = 0; at < source.size();)
                {
                    const std::size_t lineEnd = source.find('\n', at);
                    const std::string_view candidate = std::string_view(source).substr(
                        at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at);
                    if (candidate == wgslStage || candidate == vertexStage)
                    {
                        source.resize(at);
                        break;
                    }
                    if (lineEnd == std::string::npos)
                    {
                        break;
                    }
                    at = lineEnd + 1;
                }
                return true;
            }
            if (end == std::string::npos)
            {
                break;
            }
            begin = end + 1;
            ++line;
        }
        return false;
    }

    const ShaderParam *Shader::FindParam(const std::string_view name) const
    {
        for (const ShaderParam &param : _params)
        {
            if (param.name == name)
            {
                return &param;
            }
        }
        return nullptr;
    }

    // ===== loading =====

    void Shader::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, [] { IO::Resources::Instance().RegisterLoader(std::string(Extension), LoadShaderFromFile); });
    }

    bool Shader::Load(const std::filesystem::path &path)
    {
        const auto text = IO::Vfs::Instance().ReadText(path);
        if (!text)
        {
            Logger::Error(std::format("Cannot read shader file {}", path.string()));
            return false;
        }
        std::string normalised;
        normalised.reserve(text->size());
        for (const char c : *text)
        {
            if (c != '\r')
            {
                normalised.push_back(c);
            }
        }
        if (!Parse(normalised, path.string()))
        {
            return false;
        }
        _sourcePath = path.string();
        ++_version; // a reload: the GPU materials made from the old one are remade
        return true;
    }

    std::shared_ptr<Base::Asset> LoadShaderFromFile(const std::filesystem::path &path)
    {
        auto shader = std::make_shared<Shader>();
        if (!shader->Load(path))
        {
            return nullptr;
        }
        return shader;
    }

    // ===== cooked payloads =====

    void Shader::SetCookedDirectory(std::filesystem::path directory)
    {
        _cookedDirectory = std::move(directory);
    }

    std::filesystem::path Shader::GetCookedDirectory() const
    {
        if (!_cookedDirectory.empty())
        {
            return _cookedDirectory;
        }
        const std::filesystem::path root = IO::ResourceLoader::Instance().GetMetadataRoot();
        if (root.empty())
        {
            return {};
        }
        return root / "shaders" / GetUUID().ToString();
    }

    std::optional<Renderer::Common::UserShaderBlob> Shader::MakeBlob(const Renderer::Common::UserShaderTarget target,
                                                                     std::string &error) const
    {
        std::string cookedError;
        if (auto blob = MakeCookedBlob(target, cookedError))
        {
            return blob;
        }
        std::string why;
        if (auto blob = MakeRuntimeBlob(target, why))
        {
            return blob;
        }
        error = why.empty() ? cookedError : cookedError + "; " + why;
        return std::nullopt;
    }

    std::optional<Renderer::Common::UserShaderBlob> Shader::MakeCookedBlob(const Renderer::Common::UserShaderTarget target,
                                                                           std::string &error) const
    {
        using Renderer::Common::UserParam;
        using Renderer::Common::UserParamType;
        using Renderer::Common::UserShaderBlob;
        using Renderer::Common::UserShaderTarget;
        using Renderer::Common::UserTextureDefault;

        const std::filesystem::path directory = GetCookedDirectory();
        if (directory.empty())
        {
            error = "there is no cooked shader folder (no project is open)";
            return std::nullopt;
        }
        const auto manifestText = IO::Vfs::Instance().ReadText(directory / "manifest.json");
        if (!manifestText)
        {
            error = std::format("no cooked data at {} (cook it with tools/n2shader; the project's build does)",
                                (directory / "manifest.json").string());
            return std::nullopt;
        }
        nlohmann::json manifest;
        try
        {
            manifest = nlohmann::json::parse(*manifestText);
        }
        catch (const std::exception &e)
        {
            error = std::format("the cooked manifest is not valid JSON: {}", e.what());
            return std::nullopt;
        }

        return BlobFromManifest(
            manifest,
            [&](const std::string &file) -> std::expected<std::vector<std::uint8_t>, std::string>
            {
                auto bytes = IO::Vfs::Instance().ReadBytes(directory / file);
                if (!bytes)
                {
                    return std::unexpected(bytes.error());
                }
                return std::move(*bytes);
            },
            target, error);
    }

    std::optional<Renderer::Common::UserShaderBlob> Shader::BlobFromManifest(const nlohmann::json &manifest, const PayloadReader &readPayload,
                                                                             const Renderer::Common::UserShaderTarget target,
                                                                             std::string &error) const
    {
        using Renderer::Common::UserParam;
        using Renderer::Common::UserParamType;
        using Renderer::Common::UserShaderBlob;
        using Renderer::Common::UserShaderTarget;
        using Renderer::Common::UserTextureDefault;

        try
        {
            if (manifest.at("formatVersion").get<int>() != 1 ||
                manifest.at("contract").get<std::uint32_t>() != Renderer::Common::kUserShaderContractVersion)
            {
                error = std::format("the cooked shader is for contract {} (format {}); this engine supports contract {}, format 1",
                                    manifest.at("contract").dump(), manifest.at("formatVersion").dump(),
                                    Renderer::Common::kUserShaderContractVersion);
                return std::nullopt;
            }

            // The cooked parameters must be the header's: same names and types in the same order. A shader edited since it was cooked
            // is stale, and drawing it with the old block layout would write the wrong bytes
            const nlohmann::json &cooked = manifest.at("params");
            bool same = cooked.is_array() && cooked.size() == _params.size();
            for (std::size_t i = 0; same && i < _params.size(); ++i)
            {
                same = cooked[i].at("name").get<std::string>() == _params[i].name &&
                    cooked[i].at("type").get<std::string>() == ParamTypeName(_params[i].type);
            }
            if (!same)
            {
                error = "the cooked data is stale: its parameters are not the ones the .n2shader declares now (cook it again)";
                return std::nullopt;
            }

            UserShaderBlob blob;
            blob.name = _name;
            blob.sourcePath = _sourcePath;
            blob.contract = manifest.at("contract").get<std::uint32_t>();
            blob.blockSize = manifest.at("blockSize").get<std::uint32_t>();
            for (const nlohmann::json &use : manifest.at("uses"))
            {
                blob.usesLighting = blob.usesLighting || use.get<std::string>() == "lighting";
            }
            if (blob.usesLighting != _usesLighting)
            {
                error = "the cooked data is stale: it does not read the lighting as the .n2shader says now (cook it again)";
                return std::nullopt;
            }
            for (std::size_t i = 0; i < _params.size(); ++i)
            {
                const nlohmann::json &entry = cooked[i];
                const ShaderParam &declared = _params[i];
                UserParam param;
                param.name = declared.name;
                switch (declared.type)
                {
                case ShaderParamType::Float: param.type = UserParamType::Float; break;
                case ShaderParamType::Int: param.type = UserParamType::Int; break;
                case ShaderParamType::Bool: param.type = UserParamType::Bool; break;
                case ShaderParamType::Vec2: param.type = UserParamType::Vec2; break;
                case ShaderParamType::Vec3: param.type = UserParamType::Vec3; break;
                case ShaderParamType::Vec4: param.type = UserParamType::Vec4; break;
                case ShaderParamType::Color: param.type = UserParamType::Color; break;
                case ShaderParamType::Texture: param.type = UserParamType::Texture; break;
                case ShaderParamType::Mat3: param.type = UserParamType::Mat3; break;
                case ShaderParamType::Mat4: param.type = UserParamType::Mat4; break;
                }
                if (declared.type == ShaderParamType::Texture)
                {
                    param.slot = entry.at("slot").get<std::uint32_t>();
                    param.textureDefault = declared.defaultTexture == "black" ? UserTextureDefault::Black
                        : declared.defaultTexture == "normal"                 ? UserTextureDefault::Normal
                        : declared.defaultTexture == "none"                   ? UserTextureDefault::None
                                                                              : UserTextureDefault::White;
                }
                else
                {
                    param.offset = entry.at("offset").get<std::uint32_t>();
                    param.size = entry.at("size").get<std::uint32_t>();
                    param.defaultValue = declared.defaultValue;
                }
                blob.params.push_back(std::move(param));
            }

            const char *key = target == UserShaderTarget::Vulkan ? "vulkan" : target == UserShaderTarget::WebGpu ? "webgpu" : "gl";
            const nlohmann::json &payloads = manifest.at("payloads");
            if (!payloads.contains(key))
            {
                error = std::format("the cooked data has no payload for the {} backend (it was cooked for: {})", key,
                                    [&] { std::string list; for (const auto &[name, value] : payloads.items()) { list += (list.empty() ? "" : ", ") + name; } return list; }());
                return std::nullopt;
            }
            const auto payload = readPayload(payloads.at(key).at("file").get<std::string>());
            if (!payload)
            {
                error = std::format("the cooked {} payload cannot be read: {}", key, payload.error());
                return std::nullopt;
            }
            const auto toWords = [](const std::vector<std::uint8_t> &bytes, std::vector<std::uint32_t> &words) {
                words.resize(bytes.size() / 4);
                for (std::size_t i = 0; i < words.size(); ++i)
                {
                    const std::uint8_t *b = bytes.data() + 4 * i;
                    words[i] = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                        (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
                }
            };
            const auto toText = [](const std::vector<std::uint8_t> &bytes) {
                std::string text;
                text.reserve(bytes.size());
                for (const std::uint8_t byte : bytes)
                {
                    if (byte != '\r')
                    {
                        text.push_back(static_cast<char>(byte)); // the cooked text is LF; a checkout may have converted it
                    }
                }
                return text;
            };
            if (target == UserShaderTarget::Vulkan)
            {
                if (payload->size() % 4 != 0)
                {
                    error = "the cooked SPIR-V is not a whole number of words";
                    return std::nullopt;
                }
                toWords(*payload, blob.fragmentSpirv);
            }
            else
            {
                (target == UserShaderTarget::WebGpu ? blob.fragmentWgsl : blob.fragmentGlsl) = toText(*payload);
            }

            // A custom vertex stage (stage 9d): the manifest and the header must agree, as they do for the parameters
            if (manifest.value("vertexStage", false) != _hasVertexStage)
            {
                error = _hasVertexStage
                    ? "the cooked data is stale: the .n2shader has a vertex stage now and it was cooked without one (cook it again)"
                    : "the cooked data is stale: it has a vertex stage the .n2shader no longer has (cook it again)";
                return std::nullopt;
            }
            if (_hasVertexStage)
            {
                const std::string vertexKey = std::string(key) + "Vertex";
                if (!payloads.contains(vertexKey))
                {
                    error = std::format("the cooked data has no vertex payload for the {} backend", key);
                    return std::nullopt;
                }
                const auto vertex = readPayload(payloads.at(vertexKey).at("file").get<std::string>());
                if (!vertex)
                {
                    error = std::format("the cooked {} payload cannot be read: {}", vertexKey, vertex.error());
                    return std::nullopt;
                }
                if (target == UserShaderTarget::Vulkan)
                {
                    if (vertex->size() % 4 != 0)
                    {
                        error = "the cooked vertex SPIR-V is not a whole number of words";
                        return std::nullopt;
                    }
                    toWords(*vertex, blob.vertexSpirv);
                }
                else
                {
                    (target == UserShaderTarget::WebGpu ? blob.vertexWgsl : blob.vertexGlsl) = toText(*vertex);
                }
            }
            return blob;
        }
        catch (const nlohmann::json::exception &e)
        {
            error = std::format("the cooked manifest is malformed: {}", e.what());
            return std::nullopt;
        }
    }
    // ===== compiling here =====

#if !defined(__EMSCRIPTEN__)
    namespace
    {
        constexpr std::size_t kMaxCacheEntries = 256;

        std::optional<std::vector<std::uint8_t>> ReadFileBytes(const std::filesystem::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return bytes;
        }

        /// Through a temporary file renamed over the target, so a crash never leaves half an entry
        bool WriteFileAtomically(const std::filesystem::path &file, const void *data, const std::size_t size)
        {
            std::filesystem::path temporary = file;
            temporary += ".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    return false;
                }
                out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
                if (!out)
                {
                    return false;
                }
            }
            std::error_code error;
            std::filesystem::rename(temporary, file, error);
            if (error)
            {
                std::filesystem::remove(temporary, error);
                return false;
            }
            return true;
        }

        /// Keeps the cache bounded: past kMaxCacheEntries folders the least recently written go (an entry is advisory: gone means
        /// compiled again)
        void PruneCache(const std::filesystem::path &cache)
        {
            std::error_code error;
            std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;
            for (const auto &entry : std::filesystem::directory_iterator(cache, error))
            {
                if (entry.is_directory(error))
                {
                    entries.emplace_back(entry.last_write_time(error), entry.path());
                }
            }
            if (entries.size() <= kMaxCacheEntries)
            {
                return;
            }
            std::ranges::sort(entries);
            for (std::size_t i = 0; i + kMaxCacheEntries < entries.size(); ++i)
            {
                std::filesystem::remove_all(entries[i].second, error);
            }
        }
    }

#endif

    std::optional<Renderer::Common::UserShaderBlob> Shader::MakeRuntimeBlob(const Renderer::Common::UserShaderTarget target,
                                                                            std::string &why) const
    {
#if defined(__EMSCRIPTEN__)
        (void)target;
        why = "the web ships cooked payloads (the C++ twin of the cook is not in the web build)";
        return std::nullopt;
#else
        using Renderer::Common::UserShaderTarget;
        if (_hasVertexStage)
        {
            why = "it has a custom vertex stage, which is only made by the cook (tools/n2shader; the project's build and the editor do it)";
            return std::nullopt;
        }
        if (target == UserShaderTarget::WebGpu)
        {
            why = "WGSL is made by the cook (Tint is not linked into the engine): the web ships cooked WGSL";
            return std::nullopt;
        }
        if (target == UserShaderTarget::Vulkan)
        {
            if (!UserShaderCompiler::IsAvailable())
            {
                why = "it cannot be compiled here: this build has no glslang (N2ENGINE_RUNTIME_SHADER_COMPILER)";
                return std::nullopt;
            }
            if (!RenderSettings::GetRuntimeShaderCompile())
            {
                why = "compiling shaders on the device is off (the project setting rendering.runtimeShaderCompile)";
                return std::nullopt;
            }
        }
        std::string stage;
        int firstLine = 0;
        if (!GetStage(stage, firstLine) || Trimmed(stage).empty())
        {
            why = "its source is not here (a pack made with stripped shader sources), so it cannot be compiled on this machine";
            return std::nullopt;
        }

        const char *payloadKey = target == UserShaderTarget::Vulkan ? "vulkan" : "gl";
        const std::filesystem::path cache = RenderSettings::GetShaderCacheDirectory();
        const std::string key = UserShaderCompiler::CacheKey(target, *this);
        const std::filesystem::path entry = cache.empty() ? std::filesystem::path() : cache / key;

        // The cache first: an entry is used only when the payload is the one its manifest hashed
        if (!entry.empty())
        {
            std::string ignored;
            if (const auto manifestBytes = ReadFileBytes(entry / "manifest.json"))
            {
                try
                {
                    const nlohmann::json manifest = nlohmann::json::parse(manifestBytes->begin(), manifestBytes->end());
                    const nlohmann::json &payload = manifest.at("payloads").at(payloadKey);
                    const auto bytes = ReadFileBytes(entry / payload.at("file").get<std::string>());
                    if (bytes && UserShaderCompiler::Sha256Hex(bytes->data(), bytes->size()) == payload.at("sha256").get<std::string>())
                    {
                        auto blob = BlobFromManifest(
                            manifest, [&](const std::string &) -> std::expected<std::vector<std::uint8_t>, std::string> { return *bytes; },
                            target, ignored);
                        if (blob)
                        {
                            return blob;
                        }
                    }
                }
                catch (const std::exception &)
                {
                    // a damaged entry is compiled again (and overwritten below)
                }
            }
        }

        const std::string cite = std::filesystem::path(_sourcePath).filename().generic_string();
        auto compiled = UserShaderCompiler::Compile(target, *this, cite.empty() ? "shader.n2shader" : cite);
        if (!compiled)
        {
            why = "compiling it here failed:\n" + compiled.error();
            return std::nullopt;
        }
        if (!entry.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(entry, error);
            if (!error && WriteFileAtomically(entry / compiled->payloadFile, compiled->payload.data(), compiled->payload.size()) &&
                WriteFileAtomically(entry / "manifest.json", compiled->manifest.data(), compiled->manifest.size()))
            {
                PruneCache(cache);
            }
        }
        std::string manifestError;
        try
        {
            const nlohmann::json manifest = nlohmann::json::parse(compiled->manifest);
            auto blob = BlobFromManifest(
                manifest, [&](const std::string &) -> std::expected<std::vector<std::uint8_t>, std::string> { return compiled->payload; },
                target, manifestError);
            if (blob)
            {
                return blob;
            }
        }
        catch (const std::exception &e)
        {
            manifestError = e.what();
        }
        why = "what was compiled here does not fit the header: " + manifestError;
        return std::nullopt;
#endif
    }
}
