#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <renderer/common/UserShader.hpp>

namespace N2Engine::Rendering
{
    class Shader;

    /**
     * The C++ twin of the cook (tools/n2shader), for shaders that have no usable cooked payload on this machine (orchestrator/proposals/
     * user-shaders.md, stage 7). It builds the same GLSL the cook builds (the prelude templates of renderer/shaders/user/prelude, embedded
     * at configure time, the generated Material block, the author's stage behind a #line) and, for Vulkan, compiles it with the glslang
     * that N2ENGINE_RUNTIME_SHADER_COMPILER links (the same flags as `glslangValidator -V --target-env vulkan1.1`), then checks what
     * glslang reflected against the layout the header implies. OpenGL needs no compiler: its payload is the text.
     *
     * A custom vertex stage (stage 9d) and the cube and 3D atlas lookups (stage 9e) are built too, with the same templates the
     * cook uses, so such a shader also draws without cooked data where the runtime compile is on.
     *
     * Not done here, by design: the cook's lint (an authoring aid; what protects a player is the size limits and
     * ValidateUserShaderBlob's SPIR-V allow-list) and WGSL (Tint is not linked: a web player ships cooked WGSL).
     */
    namespace UserShaderCompiler
    {
        /// True when this build links glslang (N2ENGINE_RUNTIME_SHADER_COMPILER), so Vulkan payloads can be made
        [[nodiscard]] bool IsAvailable();

        /// The linked compiler as `glslangValidator --version` prints it ("Glslang Version: 11:15.3.0"); empty without one
        [[nodiscard]] std::string CompilerId();

        /// The complete GLSL of one target (Vulkan, WebGpu or OpenGl), exactly what `n2shader prelude` prints for it. `citePath` is
        /// what #line cites. nullopt, with `error` set, when the file has no `#stage fragment` line or a template is missing. Pure
        /// text: it works in every build
        [[nodiscard]] std::optional<std::string> BuildSource(Renderer::Common::UserShaderTarget target, const Shader &shader,
                                                             std::string_view citePath, std::string &error);

        /// The complete GLSL of one target's custom vertex stage (stage 9d), exactly what the cook's buildVertexSource makes:
        /// the vertex prelude, the Material block (no textures), the author's n2_vertex() behind a #line and the generated main.
        /// nullopt, with `error` set, for a shader without a `#stage vertex` block
        [[nodiscard]] std::optional<std::string> BuildVertexSource(Renderer::Common::UserShaderTarget target, const Shader &shader,
                                                                   std::string_view citePath, std::string &error);

        /// One compiled target: the manifest the engine's cooked-data reader takes, the payload file's name and bytes
        struct Compiled
        {
            std::string manifest;
            std::string payloadFile;
            std::vector<std::uint8_t> payload;
            /// A custom vertex stage's payload (shader.vulkan.vert.spv or shader.gl.vert); empty file name without one
            std::string vertexPayloadFile;
            std::vector<std::uint8_t> vertexPayload;
        };

        /// Compiles one target (Vulkan needs IsAvailable(); OpenGl always works; WebGpu is refused). The error is a list of
        /// diagnostics, one per line, in the cook's form ("assets/x.n2shader:12: error: ...")
        [[nodiscard]] std::expected<Compiled, std::string> Compile(Renderer::Common::UserShaderTarget target, const Shader &shader,
                                                                   std::string_view citePath);

        /// What identifies a compile's input: the file's text, the prelude templates, the contract, the compiler and the target. A
        /// different engine, prelude or glslang is a different key, so a stale cache entry is never read
        [[nodiscard]] std::string CacheKey(Renderer::Common::UserShaderTarget target, const Shader &shader);

        /// SHA-256 as 64 lower-case hex digits (exposed for the cache's integrity check and the tests)
        [[nodiscard]] std::string Sha256Hex(const void *data, std::size_t size);
    }
}
