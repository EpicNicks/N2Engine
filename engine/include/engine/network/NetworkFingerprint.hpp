#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/network/InputLayout.hpp"
#include "engine/network/PrefabTable.hpp"

namespace N2Engine::Network
{
    /// The longest engine version text a fingerprint carries
    inline constexpr std::size_t kMaxEngineVersionLength = 32;
    /// The longest scene path a fingerprint carries (informational: it is not compared)
    inline constexpr std::size_t kMaxScenePathLength = 255;

    /// Why a client refuses to join a server (also what a client tells the server when it leaves for that reason).
    /// The numbers are the wire's.
    enum class JoinRefusal : std::uint8_t
    {
        None = 0,
        /// The server speaks another network wire version
        WireVersion = 1,
        /// Another engine version
        EngineVersion = 2,
        /// The scene's content differs (its content hash)
        SceneContent = 3,
        /// The registered component types or the prefab table differ
        Registry = 4,
        /// The scenes have a different number of NetworkObjects
        ObjectCount = 5,
        /// A Deterministic server and a client built differently: compiler, strict floating point, contraction, Debug
        /// or Release, or architecture (BuildProfile). Never refused for the other presets.
        BuildProfile = 6,
    };

    /// "none", "wire-version", "engine-version", "scene-content", "registry", "object-count" or "build-profile"
    [[nodiscard]] std::string_view ToString(JoinRefusal refusal) noexcept;

    /**
     * What the server sends first of all and the client checks before it takes part in anything: the build and the
     * world both sides must agree on. A client that finds any difference refuses to join, with an error that names it.
     * (The server also checks what the client says it has in its accept, but a client can claim anything: the check
     * that protects a player from a wrong build is the client's own.)
     *
     * The fingerprint covers: the engine version text; the content of the scene (SceneHash: HashSceneJson of the scene
     * file's parsed JSON, so line endings and key order do not matter); the registered component type names, the
     * replicated fields of each and how they are encoded, the prefab table and the networked input layout (RegistryHash;
     * the input layout enters as InputLayout::Hash(), so a client whose controls or their quantisation differ refuses to
     * join); the number of
     * NetworkObjects in the scene. It does not cover Lua script sources,
     * assets or the values of component fields: two peers whose scripts differ will agree here and still disagree
     * later. The wire version is a separate field of the hello message (see NetworkProtocol.hpp).
     */
    /// The build facts that decide whether two peers compute the same floating-point results, packed in a u32 (the
    /// wire's): bits 0-7 compiler id (BuildCompiler), 8-15 the compiler's major version, 16 strict-FP build, 17 engine
    /// trig contraction-free, 18 Debug build (no NDEBUG), 19 64-bit pointers, 20-23 architecture (BuildArch).
    using BuildProfileBits = std::uint32_t;

    enum class BuildCompiler : std::uint8_t { Unknown = 0, Msvc = 1, Clang = 2, Gcc = 3, Emscripten = 4 };
    enum class BuildArch : std::uint8_t { Unknown = 0, X86_64 = 1, X86 = 2, Arm64 = 3, Arm = 4, Wasm = 5 };

    /// The profile of this build
    [[nodiscard]] BuildProfileBits CurrentBuildProfile() noexcept;

    /// Text of a profile ("msvc 19 strict-fp contraction-free release x86_64 64-bit"), for errors
    [[nodiscard]] std::string DescribeBuildProfile(BuildProfileBits profile);

    /// The first axis on which two profiles differ ("compiler", "compiler version", "strict floating point",
    /// "floating-point contraction", "Debug/Release", "pointer width", "architecture"); empty when equal
    [[nodiscard]] std::string BuildProfileDifference(BuildProfileBits a, BuildProfileBits b);

    struct Fingerprint
    {
        std::string engineVersion;
        std::uint64_t sceneHash = 0;
        std::uint64_t registryHash = 0;
        /// For error messages and logs only: never compared, since a path says nothing about content
        std::string scenePath;
        std::uint32_t sceneObjectCount = 0;
        std::uint16_t prefabCount = 0;
        /// CurrentBuildProfile() of the peer
        BuildProfileBits buildProfile = 0;
        /// Set by a Deterministic server (and meaningful only in its hello): the build profiles must match. A client
        /// compares only when the server's fingerprint says so; Authoritative sessions tolerate mixed builds.
        bool buildProfileEnforced = false;
    };

    /// FNV-1a over the compact text of `scene` (nlohmann keeps object keys sorted, so the same data hashes the same
    /// whatever order its file listed the keys in), written without recursion (JsonLimits::IterativeDump). A string
    /// that is not valid UTF-8 is replaced, never an exception.
    [[nodiscard]] std::uint64_t HashSceneJson(const nlohmann::json &scene);

    /// FNV-1a over the sorted names of every registered component type (ComponentRegistry), for each the replicated fields
    /// its schema lists with everything that decides their bytes on the wire (HashFieldFormat: name, kind, step, range,
    /// enum options), `prefabs.Hash()` and `inputLayout.Hash()` (an empty layout hashes too). A server and a client that replicate other fields, or the same fields another
    /// way, refuse each other (Registry).
    [[nodiscard]] std::uint64_t HashRegistry(const PrefabTable &prefabs, const InputLayout &inputLayout = InputLayout());

    /// `text` cut to `maxLength` bytes with every byte outside printable ASCII (0x20 to 0x7E) replaced by '?': what
    /// goes on the wire and into log lines
    [[nodiscard]] std::string SanitizeText(std::string_view text, std::size_t maxLength);

    /// The fingerprint of this process for `scenePath` (an informational text) whose parsed content hashes to
    /// `sceneHash` and which holds `sceneObjectCount` NetworkObjects, with the prefab table `prefabs`
    [[nodiscard]] Fingerprint MakeFingerprint(std::string_view scenePath, std::uint64_t sceneHash,
                                              std::uint32_t sceneObjectCount, const PrefabTable &prefabs,
                                              const InputLayout &inputLayout = InputLayout(),
                                              bool enforceBuildProfile = false);

    /// What differs between the server's fingerprint and the client's, the first of: the engine version, the scene
    /// content, the registry, the object count, and, when the server enforces it, the build profile. None when they agree.
    [[nodiscard]] JoinRefusal CompareFingerprints(const Fingerprint &server, const Fingerprint &client) noexcept;

    /// One sentence saying what differs, for the client's error and the logs
    [[nodiscard]] std::string DescribeRefusal(JoinRefusal refusal, const Fingerprint &server, const Fingerprint &client);
}
