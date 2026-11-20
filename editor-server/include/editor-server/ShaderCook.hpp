#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace N2Engine::Editor::ShaderCook
{
    /// How the host runs the shader cook (tools/n2shader, orchestrator/proposals/user-shaders.md stage 6): Node, and the tool's script.
    /// Neither is part of the host: Node is found through N2ENGINE_NODE, then PATH; the script through N2SHADER_TOOL, then a
    /// `tools/n2shader/n2shader.mjs` in the folder of the host's executable or one of the folders above it (the engine checkout).
    /// When either is missing `unavailable` says what to set; the host then keeps editing and a shader draws the editor's magenta
    /// fallback (the runtime compile of stage 7 is the other way to a payload)
    struct Tooling
    {
        std::filesystem::path node;
        std::filesystem::path script;
        std::string unavailable; ///< empty when the cook can run
        [[nodiscard]] bool Available() const { return unavailable.empty(); }
    };

    /// Looks for Node and the script. `searchFrom` are the folders whose ancestors are searched for the script (the executable's
    /// folder, the working folder)
    [[nodiscard]] Tooling FindTooling(const std::vector<std::filesystem::path> &searchFrom);

    /// What one cook of one .n2shader said
    struct CookResult
    {
        bool ok = false;
        bool ran = false;                ///< false when the tool could not be started (see `lines`)
        int exitCode = -1;
        std::vector<std::string> lines;  ///< the tool's output, one diagnostic per line ("assets/x.n2shader:12: error: [glslang] ...")
        std::string summary;             ///< one line for a log: "cooked for vulkan" or the first error
    };

    /// Runs `node n2shader.mjs cook|check <file> --cite <assetPath> ...`: a diagnostic cites the project-relative path
    /// ("assets/x.n2shader"), not where the project is on this machine. `targets` is a comma list (vulkan, webgpu, gl). A non-empty `outDir` cooks into it;
    /// empty only checks (nothing is written). Tools the targets need that are not installed are errors (--require-tools).
    [[nodiscard]] CookResult Cook(const Tooling &tooling, const std::filesystem::path &projectRoot, const std::string &assetPath,
                                  const std::string &targets, const std::filesystem::path &outDir,
                                  std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));

    /// Runs a program with arguments; its stdout and stderr are captured together. Kills it after `timeout`. Returns nullopt when it
    /// could not be started; `error` says why
    struct ProcessOutput
    {
        int exitCode = -1;
        bool timedOut = false;
        std::string output;
    };
    [[nodiscard]] std::optional<ProcessOutput> RunProcess(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                                                          std::chrono::milliseconds timeout, std::string &error);
}
