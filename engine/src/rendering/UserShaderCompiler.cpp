#include "engine/rendering/UserShaderCompiler.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "engine/rendering/Shader.hpp"

#if defined(N2ENGINE_RUNTIME_SHADER_COMPILER)
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#endif

namespace N2Engine::Rendering::UserShaderCompiler
{
    using Renderer::Common::UserShaderTarget;

    namespace
    {
        // ===== the prelude templates, embedded at configure time from renderer/shaders/user/prelude (engine/CMakeLists.txt) =====

        struct Template
        {
            std::string_view name;
            std::string_view text;
        };

        constexpr Template kTemplates[] = {
#include "UserShaderPrelude.inc"
        };

        /// A template's text with LF endings (a Windows checkout has CRLF; the output must not depend on it)
        std::optional<std::string> ReadTemplate(const std::string_view name)
        {
            for (const Template &entry : kTemplates)
            {
                if (entry.name == name)
                {
                    std::string text;
                    text.reserve(entry.text.size());
                    for (std::size_t i = 0; i < entry.text.size(); ++i)
                    {
                        if (entry.text[i] == '\r' && i + 1 < entry.text.size() && entry.text[i + 1] == '\n')
                        {
                            continue;
                        }
                        text.push_back(entry.text[i]);
                    }
                    return text;
                }
            }
            return std::nullopt;
        }

        constexpr int kTextureBindingBase = 1; // set 1, binding 1 + slot (contract.mjs)
        constexpr int kSamplerBindingBase = 6; // set 1, binding 6 + slot (WebGPU only)

        // ===== SHA-256 =====

        struct Sha256
        {
            std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            std::array<std::uint8_t, 64> block{};
            std::size_t blockSize = 0;
            std::uint64_t total = 0;

            static std::uint32_t Rotr(const std::uint32_t x, const int n) { return (x >> n) | (x << (32 - n)); }

            void Compress(const std::uint8_t *chunk)
            {
                static constexpr std::array<std::uint32_t, 64> k = {
                    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
                    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
                    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
                    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
                std::array<std::uint32_t, 64> w{};
                for (int i = 0; i < 16; ++i)
                {
                    w[i] = (static_cast<std::uint32_t>(chunk[4 * i]) << 24) | (static_cast<std::uint32_t>(chunk[4 * i + 1]) << 16) |
                        (static_cast<std::uint32_t>(chunk[4 * i + 2]) << 8) | static_cast<std::uint32_t>(chunk[4 * i + 3]);
                }
                for (int i = 16; i < 64; ++i)
                {
                    const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
                for (int i = 0; i < 64; ++i)
                {
                    const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                    const std::uint32_t ch = (e & f) ^ (~e & g);
                    const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
                    const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t t2 = s0 + maj;
                    h = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
                state[5] += f;
                state[6] += g;
                state[7] += h;
            }

            void Update(const void *data, std::size_t size)
            {
                const auto *bytes = static_cast<const std::uint8_t *>(data);
                total += size;
                while (size > 0)
                {
                    const std::size_t take = std::min(size, block.size() - blockSize);
                    std::memcpy(block.data() + blockSize, bytes, take);
                    blockSize += take;
                    bytes += take;
                    size -= take;
                    if (blockSize == block.size())
                    {
                        Compress(block.data());
                        blockSize = 0;
                    }
                }
            }

            void Update(const std::string_view text) { Update(text.data(), text.size()); }

            std::string Finish()
            {
                const std::uint64_t bits = total * 8;
                const std::uint8_t one = 0x80;
                Update(&one, 1);
                const std::uint8_t zero = 0;
                while (blockSize != 56)
                {
                    Update(&zero, 1);
                }
                std::array<std::uint8_t, 8> length{};
                for (int i = 0; i < 8; ++i)
                {
                    length[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
                }
                Update(length.data(), length.size());
                std::string hex;
                for (const std::uint32_t word : state)
                {
                    hex += std::format("{:08x}", word);
                }
                return hex;
            }
        };

        // ===== the Material block (std140), as tools/n2shader/lib/std140.mjs =====

        struct Member
        {
            const ShaderParam *param = nullptr;
            const char *glsl = "";
            std::uint32_t offset = 0;
            std::uint32_t size = 0;
        };

        struct BlockLayout
        {
            std::vector<Member> members;
            std::uint32_t size = 0;
        };

        std::uint32_t AlignUp(const std::uint32_t value, const std::uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        BlockLayout LayoutBlock(const std::vector<ShaderParam> &params)
        {
            BlockLayout layout;
            std::uint32_t cursor = 0;
            for (const ShaderParam &param : params)
            {
                Member member;
                member.param = &param;
                std::uint32_t align = 4;
                switch (param.type)
                {
                case ShaderParamType::Float: member.glsl = "float"; member.size = 4; break;
                case ShaderParamType::Int: member.glsl = "int"; member.size = 4; break;
                case ShaderParamType::Bool: member.glsl = "int"; member.size = 4; break; // a bool is an int in a uniform block
                case ShaderParamType::Vec2: member.glsl = "vec2"; member.size = 8; align = 8; break;
                case ShaderParamType::Vec3: member.glsl = "vec3"; member.size = 12; align = 16; break;
                case ShaderParamType::Vec4:
                case ShaderParamType::Color: member.glsl = "vec4"; member.size = 16; align = 16; break;
                case ShaderParamType::Texture: continue; // a texture is not in the block
                }
                member.offset = AlignUp(cursor, align);
                cursor = member.offset + member.size;
                layout.members.push_back(member);
            }
            layout.size = layout.members.empty() ? 0 : AlignUp(cursor, 16);
            return layout;
        }

        const char *TypeName(const ShaderParamType type)
        {
            static constexpr std::array<const char *, 8> names{"float", "int", "bool", "vec2", "vec3", "vec4", "color", "texture"};
            return names[static_cast<std::size_t>(type)];
        }

        // ===== the generated text, as tools/n2shader/lib/prelude.mjs =====

        std::vector<std::string> SplitLines(const std::string &text)
        {
            std::vector<std::string> lines;
            std::size_t begin = 0;
            for (;;)
            {
                const std::size_t end = text.find('\n', begin);
                if (end == std::string::npos)
                {
                    lines.push_back(text.substr(begin));
                    return lines;
                }
                lines.push_back(text.substr(begin, end - begin));
                begin = end + 1;
            }
        }

        std::string Join(const std::vector<std::string> &lines)
        {
            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (i > 0)
                {
                    out.push_back('\n');
                }
                out += lines[i];
            }
            return out;
        }

        std::string TrimSpaces(const std::string &text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
            {
                ++begin;
            }
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        std::string ReplaceAll(std::string text, const std::string_view from, const std::string &to)
        {
            for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            {
                text.replace(at, from.size(), to);
            }
            return text;
        }

        /// Expands //@include, //@if-lighting and //@each-texture; the same algorithm as expandTemplate, line for line
        bool ExpandTemplate(const std::string &text, const bool lighting, const std::vector<int> &slots, const int depth, std::string &out,
                            std::string &error)
        {
            if (depth > 4)
            {
                error = "prelude includes nest too deeply";
                return false;
            }
            std::vector<std::string> result;
            const std::vector<std::string> lines = SplitLines(text);
            for (std::size_t index = 0; index < lines.size(); ++index)
            {
                const std::string &line = lines[index];
                const std::string trimmed = TrimSpaces(line);
                constexpr std::string_view includePrefix = "//@include";
                if (trimmed.starts_with(includePrefix) && trimmed.size() > includePrefix.size() &&
                    (trimmed[includePrefix.size()] == ' ' || trimmed[includePrefix.size()] == '\t'))
                {
                    const std::string name = TrimSpaces(trimmed.substr(includePrefix.size()));
                    if (!name.empty() && name.find_first_of(" \t") == std::string::npos)
                    {
                        const auto included = ReadTemplate(name);
                        if (!included)
                        {
                            error = "prelude template: no template named " + name;
                            return false;
                        }
                        std::string expanded;
                        if (!ExpandTemplate(*included, lighting, slots, depth + 1, expanded, error))
                        {
                            return false;
                        }
                        std::vector<std::string> expandedLines = SplitLines(expanded);
                        expandedLines.pop_back();
                        result.insert(result.end(), expandedLines.begin(), expandedLines.end());
                        continue;
                    }
                }
                const bool isIf = trimmed == "//@if-lighting";
                const bool isEach = trimmed == "//@each-texture";
                if (!isIf && !isEach)
                {
                    result.push_back(line);
                    continue;
                }
                const std::string closer = isIf ? "//@end-if" : "//@end-each";
                std::size_t end = index + 1;
                while (end < lines.size() && TrimSpaces(lines[end]) != closer)
                {
                    ++end;
                }
                if (end >= lines.size())
                {
                    error = "prelude template: " + trimmed + " without " + closer;
                    return false;
                }
                const std::string body = Join(std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                                                                       lines.begin() + static_cast<std::ptrdiff_t>(end)));
                if (isIf)
                {
                    if (lighting)
                    {
                        std::string expanded;
                        if (!ExpandTemplate(body + "\n", lighting, slots, depth, expanded, error))
                        {
                            return false;
                        }
                        std::vector<std::string> expandedLines = SplitLines(expanded);
                        expandedLines.pop_back();
                        result.insert(result.end(), expandedLines.begin(), expandedLines.end());
                    }
                }
                else
                {
                    for (const int slot : slots)
                    {
                        std::string filled = ReplaceAll(body, "@SLOT@", std::to_string(slot));
                        filled = ReplaceAll(filled, "@TEX_BINDING@", std::to_string(kTextureBindingBase + slot));
                        filled = ReplaceAll(filled, "@SAMP_BINDING@", std::to_string(kSamplerBindingBase + slot));
                        const std::vector<std::string> filledLines = SplitLines(filled);
                        result.insert(result.end(), filledLines.begin(), filledLines.end());
                    }
                }
                index = end;
            }
            out = Join(result) + "\n";
            return true;
        }

        std::string MaterialSection(const UserShaderTarget target, const BlockLayout &layout, const std::vector<ShaderParam> &params)
        {
            std::string text = std::format(
                "// Generated from the header's params (contract {}); a bool is an int (0 or 1), a texture is its slot number for n2_texture\n",
                Renderer::Common::kUserShaderContractVersion);
            if (!layout.members.empty())
            {
                if (target == UserShaderTarget::OpenGl)
                {
                    for (const Member &member : layout.members)
                    {
                        text += std::format("uniform {} {};\n", member.glsl, member.param->name);
                    }
                }
                else
                {
                    text += "layout(set = 1, binding = 0, std140) uniform Material {\n";
                    for (const Member &member : layout.members)
                    {
                        text += std::format("    {} {};\n", member.glsl, member.param->name);
                    }
                    text += "};\n";
                }
            }
            for (const ShaderParam &param : params)
            {
                if (param.type == ShaderParamType::Texture)
                {
                    text += std::format("const int {} = {};\n", param.name, param.slot);
                }
            }
            return text;
        }

        const char *TargetName(const UserShaderTarget target)
        {
            return target == UserShaderTarget::Vulkan ? "vulkan" : target == UserShaderTarget::WebGpu ? "webgpu" : "gl";
        }

        std::string LinePath(const std::string_view file)
        {
            std::string out;
            for (const char c : file)
            {
                out.push_back(c == '\\' ? '/' : (c == '"' || c == '\r' || c == '\n') ? '_' : c);
            }
            return out;
        }

        /// The digest of every embedded template: a changed prelude is a different cache key without anyone bumping a number
        const std::string &PreludeDigest()
        {
            static const std::string digest = [] {
                Sha256 sha;
                for (const Template &entry : kTemplates)
                {
                    sha.Update(entry.name);
                    sha.Update("\n", 1);
                    sha.Update(ReadTemplate(entry.name).value_or(""));
                    sha.Update("\n", 1);
                }
                return sha.Finish();
            }();
            return digest;
        }
    }

    std::string Sha256Hex(const void *data, const std::size_t size)
    {
        Sha256 sha;
        sha.Update(data, size);
        return sha.Finish();
    }

    std::optional<std::string> BuildSource(const UserShaderTarget target, const Shader &shader, const std::string_view citePath,
                                           std::string &error)
    {
        std::string source;
        int firstLine = 0;
        if (!shader.GetStage(source, firstLine))
        {
            error = "no `#stage fragment` line: a .n2shader is a JSON header, then that line, then the GLSL";
            return std::nullopt;
        }
        std::vector<int> slots;
        for (const ShaderParam &param : shader.GetParams())
        {
            if (param.type == ShaderParamType::Texture)
            {
                slots.push_back(param.slot);
            }
        }
        std::ranges::sort(slots);
        const char *templateName = target == UserShaderTarget::Vulkan ? "vulkan.glsl" : target == UserShaderTarget::WebGpu ? "webgpu.glsl" : "opengl.glsl";
        const auto templateText = ReadTemplate(templateName);
        if (!templateText)
        {
            error = std::string("no prelude template named ") + templateName;
            return std::nullopt;
        }
        std::string prelude;
        if (!ExpandTemplate(*templateText, shader.UsesLighting(), slots, 0, prelude, error))
        {
            return std::nullopt;
        }
        const std::string head = target == UserShaderTarget::OpenGl
            ? "#version 330 core\n"
            : "#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n#extension GL_EXT_samplerless_texture_functions : enable\n";
        const std::string lineDirective = target == UserShaderTarget::OpenGl
            ? std::format("#line {} 1\n", firstLine)
            : std::format("#line {} \"{}\"\n", firstLine, LinePath(citePath));
        const BlockLayout layout = LayoutBlock(shader.GetParams());
        return std::format("{}// n2shader contract {}, target {}\n{}{}{}{}\n", head, Renderer::Common::kUserShaderContractVersion,
                           TargetName(target), prelude, MaterialSection(target, layout, shader.GetParams()), lineDirective, source);
    }

    std::string CacheKey(const UserShaderTarget target, const Shader &shader)
    {
        Sha256 sha;
        sha.Update(shader.GetFileText());
        sha.Update("\n", 1);
        sha.Update(PreludeDigest());
        sha.Update(std::format("\ncontract {}\ntarget {}\ncompiler {}\n", Renderer::Common::kUserShaderContractVersion, TargetName(target),
                               target == UserShaderTarget::Vulkan ? CompilerId() : std::string()));
        return sha.Finish();
    }

#if defined(N2ENGINE_RUNTIME_SHADER_COMPILER)

    bool IsAvailable()
    {
        return true;
    }

    std::string CompilerId()
    {
        const glslang::Version version = glslang::GetVersion();
        return std::format("Glslang Version: {}:{}.{}.{}{}", glslang::GetSpirvGeneratorVersion(), version.major, version.minor, version.patch,
                           version.flavor != nullptr ? version.flavor : "");
    }

    namespace
    {
        std::mutex &CompilerMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        void EnsureProcess()
        {
            static std::once_flag once;
            std::call_once(once, [] { glslang::InitializeProcess(); });
        }

        /// glslang's log without its blank lines, its warnings (the cook lets them pass too) and its "N compilation errors" summary
        std::string TrimLog(const std::string &log)
        {
            std::string out;
            std::istringstream stream(log);
            std::string line;
            while (std::getline(stream, line))
            {
                line = TrimSpaces(line);
                if (line.empty() || line.starts_with("WARNING:") || line.find("compilation error") != std::string::npos)
                {
                    continue;
                }
                if (!out.empty())
                {
                    out.push_back('\n');
                }
                out += line;
            }
            return out;
        }

        /// glslang's reflection of the Material block against the layout the header implies (the cook's reflection self-check)
        std::optional<std::string> CheckMaterialBlock(glslang::TProgram &program, const BlockLayout &layout)
        {
            int blockIndex = -1;
            for (int i = 0; i < program.getNumUniformBlocks(); ++i)
            {
                if (program.getUniformBlock(i).name == "Material")
                {
                    blockIndex = i;
                }
            }
            if (layout.members.empty())
            {
                return blockIndex >= 0 ? std::optional<std::string>("the Material block is there but the header declares no non-texture parameter") : std::nullopt;
            }
            if (blockIndex < 0)
            {
                return std::string("the compiled shader has no Material block");
            }
            if (program.getUniformBlock(blockIndex).size != static_cast<int>(layout.size))
            {
                return std::format("the Material block is {} bytes in the compiled shader, {} by the header's layout",
                                   program.getUniformBlock(blockIndex).size, layout.size);
            }
            for (const Member &member : layout.members)
            {
                bool found = false;
                for (int i = 0; i < program.getNumUniformVariables() && !found; ++i)
                {
                    if (program.getUniformBlockIndex(i) != blockIndex)
                    {
                        continue;
                    }
                    const glslang::TObjectReflection &uniform = program.getUniform(i);
                    const std::string &name = uniform.name;
                    const std::string &wanted = member.param->name;
                    if (name == wanted || (name.size() > wanted.size() && name.ends_with(wanted) && name[name.size() - wanted.size() - 1] == '.'))
                    {
                        found = true;
                        if (uniform.offset != static_cast<int>(member.offset))
                        {
                            return std::format("{} is at byte {} in the compiled shader, {} by the header's layout", wanted, uniform.offset,
                                               member.offset);
                        }
                    }
                }
                if (!found)
                {
                    return std::format("{} is not in the compiled Material block", member.param->name);
                }
            }
            return std::nullopt;
        }

        std::expected<std::vector<std::uint32_t>, std::string> CompileVulkan(const std::string &text, const BlockLayout &layout)
        {
            const std::lock_guard lock(CompilerMutex());
            EnsureProcess();
            // The cook's flags: `glslangValidator -V --target-env vulkan1.1 -S frag` (Vulkan semantics, SPIR-V 1.3, no optimizer)
            constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
            glslang::TShader shader(EShLangFragment);
            const char *strings[1] = {text.c_str()};
            shader.setStrings(strings, 1);
            shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
            if (!shader.parse(GetDefaultResources(), 110, false, messages))
            {
                return std::unexpected(TrimLog(shader.getInfoLog()));
            }
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(messages))
            {
                return std::unexpected(TrimLog(program.getInfoLog()));
            }
            if (!program.buildReflection())
            {
                return std::unexpected("glslang could not reflect the compiled shader");
            }
            if (const auto problem = CheckMaterialBlock(program, layout))
            {
                return std::unexpected("reflection self-check: " + *problem);
            }
            std::vector<std::uint32_t> spirv;
            spv::SpvBuildLogger logger;
            glslang::SpvOptions options;
            options.disableOptimizer = true;
            glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv, &logger, &options);
            if (spirv.empty())
            {
                return std::unexpected("glslang made no SPIR-V: " + logger.getAllMessages());
            }
            return spirv;
        }
    }

#else

    bool IsAvailable()
    {
        return false;
    }

    std::string CompilerId()
    {
        return {};
    }

#endif

    std::expected<Compiled, std::string> Compile(const UserShaderTarget target, const Shader &shader, const std::string_view citePath)
    {
        if (target == UserShaderTarget::WebGpu)
        {
            return std::unexpected("WGSL is made by the cook (Tint is not linked into the engine)");
        }
        std::string error;
        const auto text = BuildSource(target, shader, citePath, error);
        if (!text)
        {
            return std::unexpected(error);
        }
        const BlockLayout layout = LayoutBlock(shader.GetParams());

        Compiled compiled;
        if (target == UserShaderTarget::OpenGl)
        {
            compiled.payloadFile = "shader.gl.frag";
            compiled.payload.assign(text->begin(), text->end());
        }
        else
        {
#if defined(N2ENGINE_RUNTIME_SHADER_COMPILER)
            auto spirv = CompileVulkan(*text, layout);
            if (!spirv)
            {
                return std::unexpected(spirv.error());
            }
            compiled.payloadFile = "shader.vulkan.frag.spv";
            compiled.payload.reserve(spirv->size() * 4);
            for (const std::uint32_t word : *spirv)
            {
                for (int shift = 0; shift < 32; shift += 8)
                {
                    compiled.payload.push_back(static_cast<std::uint8_t>(word >> shift));
                }
            }
#else
            return std::unexpected("this build has no glslang (N2ENGINE_RUNTIME_SHADER_COMPILER)");
#endif
        }

        nlohmann::json manifest;
        manifest["formatVersion"] = 1;
        manifest["contract"] = Renderer::Common::kUserShaderContractVersion;
        manifest["name"] = shader.GetName();
        manifest["runtimeCompiled"] = true;
        manifest["source"] = {{"sha256", Sha256Hex(shader.GetFileText().data(), shader.GetFileText().size())}};
        manifest["uses"] = shader.UsesLighting() ? nlohmann::json::array({"lighting"}) : nlohmann::json::array();
        manifest["blockSize"] = layout.size;
        nlohmann::json params = nlohmann::json::array();
        std::size_t memberIndex = 0;
        for (const ShaderParam &param : shader.GetParams())
        {
            nlohmann::json entry{{"name", param.name}, {"type", TypeName(param.type)}};
            if (param.type == ShaderParamType::Texture)
            {
                entry["slot"] = param.slot;
            }
            else
            {
                entry["offset"] = layout.members[memberIndex].offset;
                entry["size"] = layout.members[memberIndex].size;
                ++memberIndex;
            }
            params.push_back(std::move(entry));
        }
        manifest["params"] = std::move(params);
        manifest["payloads"][TargetName(target)] = {{"file", compiled.payloadFile},
                                                    {"bytes", compiled.payload.size()},
                                                    {"sha256", Sha256Hex(compiled.payload.data(), compiled.payload.size())}};
        if (target == UserShaderTarget::Vulkan)
        {
            manifest["compiler"] = CompilerId();
        }
        compiled.manifest = manifest.dump(2) + "\n";
        return compiled;
    }
}
