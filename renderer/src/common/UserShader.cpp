#include "renderer/common/UserShader.hpp"

#include "renderer/common/UserShaderGl.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <map>
#include <set>

namespace
{
    struct Std140Info
    {
        std::uint32_t alignment;
        std::uint32_t size;
        bool inBlock;
    };

    Std140Info InfoOf(const Renderer::Common::UserParamType type)
    {
        using Renderer::Common::UserParamType;
        switch (type)
        {
        case UserParamType::Float:
        case UserParamType::Int:
        case UserParamType::Bool: return {4, 4, true};
        case UserParamType::Vec2: return {8, 8, true};
        case UserParamType::Vec3: return {16, 12, true};
        case UserParamType::Vec4:
        case UserParamType::Color: return {16, 16, true};
        case UserParamType::Mat3: return {16, 48, true};
        case UserParamType::Mat4: return {16, 64, true};
        case UserParamType::Texture: return {0, 0, false};
        }
        return {0, 0, false};
    }

    bool IsIdentifier(const std::string& name)
    {
        if (name.empty() || name.size() > Renderer::Common::kUserShaderMaxNameBytes)
        {
            return false;
        }
        const auto first = static_cast<unsigned char>(name[0]);
        if (!(std::isalpha(first) || first == '_'))
        {
            return false;
        }
        return std::all_of(name.begin(), name.end(),
                           [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    }
}

bool Renderer::Common::ReadFragmentVaryings(const std::vector<std::uint32_t>& spirv, std::uint32_t& mask, std::string& error)
{
    constexpr std::uint32_t OpEntryPoint = 15;
    constexpr std::uint32_t OpCapability = 17;
    constexpr std::uint32_t OpVariable = 59;
    constexpr std::uint32_t OpDecorate = 71;
    constexpr std::uint32_t DecorationLocation = 30;
    constexpr std::uint32_t StorageInput = 1;
    constexpr std::uint32_t ExecutionFragment = 4;
    // Matrix, Shader, Float16, Int8, Int16, Sampled1D, SampledBuffer, ImageQuery, DerivativeControl
    constexpr std::uint32_t allowedCapabilities[] = {0, 1, 9, 39, 22, 43, 46, 50, 51};

    mask = 0;
    constexpr std::size_t headerWords = 5;
    if (spirv.size() < headerWords)
    {
        error = "the SPIR-V module is shorter than its header";
        return false;
    }
    std::vector<std::uint32_t> interfaceIds;
    std::map<std::uint32_t, std::uint32_t> locations;  // id -> Location decoration
    std::set<std::uint32_t> inputVariables;            // ids of OpVariable with the Input storage class
    int fragmentEntryPoints = 0;
    std::size_t at = headerWords;
    while (at < spirv.size())
    {
        const std::uint32_t head = spirv[at];
        const std::uint32_t opcode = head & 0xFFFFu;
        const std::size_t words = head >> 16;
        if (words == 0 || at + words > spirv.size())
        {
            error = "the SPIR-V module has an instruction that runs past its end";
            return false;
        }
        switch (opcode)
        {
        case OpCapability:
            if (words >= 2 &&
                std::find(std::begin(allowedCapabilities), std::end(allowedCapabilities), spirv[at + 1]) ==
                    std::end(allowedCapabilities))
            {
                error = "the SPIR-V module declares capability " + std::to_string(spirv[at + 1]) +
                    ", which user shaders may not use";
                return false;
            }
            break;
        case OpEntryPoint:
            if (words >= 4 && spirv[at + 1] == ExecutionFragment)
            {
                ++fragmentEntryPoints;
                // The name is a null-terminated string packed four bytes to a word: it ends in the first word with a zero byte
                std::size_t name = at + 3;
                while (name < at + words)
                {
                    const std::uint32_t word = spirv[name++];
                    if ((word & 0xFFu) == 0 || (word & 0xFF00u) == 0 || (word & 0xFF0000u) == 0 || (word & 0xFF000000u) == 0)
                    {
                        break;
                    }
                }
                interfaceIds.assign(spirv.begin() + static_cast<std::ptrdiff_t>(name),
                                    spirv.begin() + static_cast<std::ptrdiff_t>(at + words));
            }
            else
            {
                error = "the SPIR-V module has an entry point that is not a fragment stage";
                return false;
            }
            break;
        case OpDecorate:
            if (words >= 4 && spirv[at + 2] == DecorationLocation)
            {
                locations[spirv[at + 1]] = spirv[at + 3];
            }
            break;
        case OpVariable:
            if (words >= 4 && spirv[at + 3] == StorageInput)
            {
                inputVariables.insert(spirv[at + 2]);
            }
            break;
        default: break;
        }
        at += words;
    }
    if (fragmentEntryPoints != 1)
    {
        error = "the SPIR-V module must have exactly one fragment entry point (it has " + std::to_string(fragmentEntryPoints) + ")";
        return false;
    }
    for (const std::uint32_t id : interfaceIds)
    {
        const auto location = locations.find(id);
        if (inputVariables.count(id) == 0 || location == locations.end())
        {
            continue; // an output, or a built-in input (gl_FragCoord): not a varying
        }
        if (location->second >= kUserVaryingCount)
        {
            error = "the fragment stage reads input location " + std::to_string(location->second) +
                "; the engine's vertex stage writes locations 0-" + std::to_string(kUserVaryingCount - 1);
            return false;
        }
        mask |= 1u << location->second;
    }
    return true;
}

bool Renderer::Common::ReadVertexInterface(const std::vector<std::uint32_t>& spirv, std::string& error)
{
    constexpr std::uint32_t OpEntryPoint = 15;
    constexpr std::uint32_t OpCapability = 17;
    constexpr std::uint32_t OpVariable = 59;
    constexpr std::uint32_t OpDecorate = 71;
    constexpr std::uint32_t DecorationLocation = 30;
    constexpr std::uint32_t StorageInput = 1;
    constexpr std::uint32_t ExecutionVertex = 0;
    constexpr std::uint32_t allowedCapabilities[] = {0, 1, 9, 39, 22, 43, 46, 50, 51};

    constexpr std::size_t headerWords = 5;
    if (spirv.size() < headerWords)
    {
        error = "the SPIR-V module is shorter than its header";
        return false;
    }
    std::map<std::uint32_t, std::uint32_t> locations; // id -> Location decoration
    std::set<std::uint32_t> inputVariables;
    int entryPoints = 0;
    std::size_t at = headerWords;
    while (at < spirv.size())
    {
        const std::uint32_t head = spirv[at];
        const std::uint32_t opcode = head & 0xFFFFu;
        const std::size_t words = head >> 16;
        if (words == 0 || at + words > spirv.size())
        {
            error = "the SPIR-V module has an instruction that runs past its end";
            return false;
        }
        switch (opcode)
        {
        case OpCapability:
            if (words >= 2 &&
                std::find(std::begin(allowedCapabilities), std::end(allowedCapabilities), spirv[at + 1]) ==
                    std::end(allowedCapabilities))
            {
                error = "the SPIR-V module declares capability " + std::to_string(spirv[at + 1]) +
                    ", which user shaders may not use";
                return false;
            }
            break;
        case OpEntryPoint:
            if (words < 4 || spirv[at + 1] != ExecutionVertex)
            {
                error = "the SPIR-V module has an entry point that is not a vertex stage";
                return false;
            }
            ++entryPoints;
            break;
        case OpDecorate:
            if (words >= 4 && spirv[at + 2] == DecorationLocation)
            {
                locations[spirv[at + 1]] = spirv[at + 3];
            }
            break;
        case OpVariable:
            if (words >= 4 && spirv[at + 3] == StorageInput)
            {
                inputVariables.insert(spirv[at + 2]);
            }
            break;
        default: break;
        }
        at += words;
    }
    if (entryPoints != 1)
    {
        error = "the SPIR-V module must have exactly one vertex entry point (it has " + std::to_string(entryPoints) + ")";
        return false;
    }
    for (const std::uint32_t id : inputVariables)
    {
        const auto location = locations.find(id);
        if (location != locations.end() && location->second >= kUserVaryingCount)
        {
            error = "the vertex stage reads attribute location " + std::to_string(location->second) +
                "; the engine's vertices have locations 0-" + std::to_string(kUserVaryingCount - 1);
            return false;
        }
    }
    return true;
}

namespace
{
    /// The checks of a blob's custom vertex payload for `target` (nothing for a blob without one). The fragment payload's checks ran first
    std::string ValidateVertexPayload(const Renderer::Common::UserShaderBlob& blob, const Renderer::Common::UserShaderTarget target)
    {
        using namespace Renderer::Common;
        switch (target)
        {
        case UserShaderTarget::Vulkan:
            if (blob.vertexSpirv.empty())
            {
                return blob.vertexWgsl.empty() && blob.vertexGlsl.empty() ? std::string() : "a vertex payload for another backend";
            }
            if (blob.vertexSpirv.size() * sizeof(std::uint32_t) > kUserShaderMaxSpirvBytes)
            {
                return "the Vulkan vertex payload is " + std::to_string(blob.vertexSpirv.size() * sizeof(std::uint32_t)) +
                    " bytes; the limit is " + std::to_string(kUserShaderMaxSpirvBytes);
            }
            if (blob.vertexSpirv[0] != 0x07230203u)
            {
                return "the Vulkan vertex payload is not SPIR-V (bad magic number)";
            }
            {
                std::string problem;
                if (!ReadVertexInterface(blob.vertexSpirv, problem))
                {
                    return "the Vulkan vertex payload: " + problem;
                }
            }
            return {};
        case UserShaderTarget::WebGpu:
            if (blob.vertexWgsl.empty())
            {
                return blob.vertexSpirv.empty() && blob.vertexGlsl.empty() ? std::string() : "a vertex payload for another backend";
            }
            if (blob.vertexWgsl.size() > kUserShaderMaxWgslBytes)
            {
                return "the WebGPU vertex payload is " + std::to_string(blob.vertexWgsl.size()) + " bytes; the limit is " +
                    std::to_string(kUserShaderMaxWgslBytes);
            }
            if (blob.vertexWgsl.find('\0') != std::string::npos)
            {
                return "the WebGPU vertex payload is not text (it holds a NUL byte)";
            }
            if (blob.vertexWgsl.find("@vertex") == std::string::npos)
            {
                return "the WebGPU vertex payload has no @vertex entry point";
            }
            return {};
        case UserShaderTarget::OpenGl:
            if (blob.vertexGlsl.empty())
            {
                return blob.vertexSpirv.empty() && blob.vertexWgsl.empty() ? std::string() : "a vertex payload for another backend";
            }
            if (std::string problem = UserGl::ValidateFragmentSource(blob.vertexGlsl); !problem.empty())
            {
                // the same text rules; the message names the fragment payload, so say which stage this was
                return "the vertex stage: " + problem;
            }
            return {};
        }
        return {};
    }
}

std::string Renderer::Common::ValidateUserShaderParams(const UserShaderBlob& blob)
{
    if (blob.contract != kUserShaderContractVersion)
    {
        return "user shader contract " + std::to_string(blob.contract) + " is not the one this engine supports (" +
            std::to_string(kUserShaderContractVersion) + ")";
    }
    if (blob.blockSize > kUserShaderMaxBlockBytes)
    {
        return "the material block is " + std::to_string(blob.blockSize) + " bytes; the limit is " +
            std::to_string(kUserShaderMaxBlockBytes);
    }
    if (blob.params.size() > kUserShaderMaxParams)
    {
        return "the shader declares " + std::to_string(blob.params.size()) + " parameters; the limit is " +
            std::to_string(kUserShaderMaxParams);
    }

    std::set<std::string> names;
    std::set<std::uint32_t> slots;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges; // [offset, end) of every block member
    for (const UserParam& param : blob.params)
    {
        if (!IsIdentifier(param.name))
        {
            return "parameter name '" + param.name + "' is not an identifier of at most " +
                std::to_string(kUserShaderMaxNameBytes) + " characters";
        }
        if (param.name.rfind("n2_", 0) == 0 || param.name.rfind("gl_", 0) == 0)
        {
            return "parameter name '" + param.name + "' starts with a reserved prefix (n2_, gl_)";
        }
        if (!names.insert(param.name).second)
        {
            return "parameter '" + param.name + "' is declared twice";
        }
        const Std140Info info = InfoOf(param.type);
        if (!info.inBlock)
        {
            if (param.slot >= kUserShaderTextureSlots)
            {
                return "texture parameter '" + param.name + "' uses slot " + std::to_string(param.slot) +
                    "; the slots are 0-" + std::to_string(kUserShaderTextureSlots - 1);
            }
            if (!slots.insert(param.slot).second)
            {
                return "texture slot " + std::to_string(param.slot) + " is used by two parameters";
            }
            continue;
        }
        if (param.size != info.size)
        {
            return "parameter '" + param.name + "' has size " + std::to_string(param.size) + " but its type takes " +
                std::to_string(info.size);
        }
        if (param.offset % info.alignment != 0)
        {
            return "parameter '" + param.name + "' is at offset " + std::to_string(param.offset) +
                ", which is not a multiple of its std140 alignment " + std::to_string(info.alignment);
        }
        if (param.offset > kUserShaderMaxBlockBytes || param.offset + param.size > blob.blockSize)
        {
            return "parameter '" + param.name + "' ends at byte " + std::to_string(param.offset + param.size) +
                ", outside the " + std::to_string(blob.blockSize) + "-byte block";
        }
        for (const auto& [begin, end] : ranges)
        {
            if (param.offset < end && begin < param.offset + param.size)
            {
                return "parameter '" + param.name + "' overlaps another parameter in the block";
            }
        }
        ranges.emplace_back(param.offset, param.offset + param.size);
    }
    return {};
}

std::string Renderer::Common::ValidateUserShaderBlob(const UserShaderBlob& blob, const UserShaderTarget target)
{
    if (std::string problem = ValidateFragmentPayload(blob, target); !problem.empty())
    {
        return problem;
    }
    return ValidateVertexPayload(blob, target);
}

std::string Renderer::Common::ValidateFragmentPayload(const UserShaderBlob& blob, const UserShaderTarget target)
{
    if (std::string problem = ValidateUserShaderParams(blob); !problem.empty())
    {
        return problem;
    }

    if (target == UserShaderTarget::OpenGl)
    {
        return UserGl::ValidateFragmentSource(blob.fragmentGlsl);
    }
    if (target == UserShaderTarget::WebGpu)
    {
        if (blob.fragmentWgsl.empty())
        {
            return "the WebGPU fragment payload is empty";
        }
        if (blob.fragmentWgsl.size() > kUserShaderMaxWgslBytes)
        {
            return "the WebGPU fragment payload is " + std::to_string(blob.fragmentWgsl.size()) + " bytes; the limit is " +
                std::to_string(kUserShaderMaxWgslBytes);
        }
        if (blob.fragmentWgsl.find('\0') != std::string::npos)
        {
            return "the WebGPU fragment payload is not text (it holds a NUL byte)";
        }
        if (blob.fragmentWgsl.find("@fragment") == std::string::npos)
        {
            return "the WebGPU fragment payload has no @fragment entry point";
        }
        return {};
    }

    if (blob.fragmentSpirv.empty())
    {
        return "the Vulkan fragment payload is empty";
    }
    if (blob.fragmentSpirv.size() * sizeof(std::uint32_t) > kUserShaderMaxSpirvBytes)
    {
        return "the Vulkan fragment payload is " + std::to_string(blob.fragmentSpirv.size() * sizeof(std::uint32_t)) +
            " bytes; the limit is " + std::to_string(kUserShaderMaxSpirvBytes);
    }
    if (blob.fragmentSpirv[0] != 0x07230203u)
    {
        return "the Vulkan fragment payload is not SPIR-V (bad magic number)";
    }
    std::uint32_t varyings = 0;
    std::string spirvProblem;
    if (!ReadFragmentVaryings(blob.fragmentSpirv, varyings, spirvProblem))
    {
        return "the Vulkan fragment payload: " + spirvProblem;
    }
    return {};
}
