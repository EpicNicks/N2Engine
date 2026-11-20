#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <renderer/common/UserShader.hpp>

#include "engine/base/Asset.hpp"

namespace N2Engine::Rendering
{
    /// The type of a declared shader parameter (the `.n2shader` header's "type", orchestrator/proposals/user-shaders.md section 3.2)
    enum class ShaderParamType : std::uint8_t
    {
        Float,
        Int,
        Bool,
        Vec2,
        Vec3,
        Vec4,
        Color, ///< a vec4 with an editor colour picker
        Texture,
        Mat3, ///< a 3x3 matrix, row-major, the identity until set (no default is declared)
        Mat4, ///< a 4x4 matrix, row-major, the identity until set
        Cube,      ///< a cube texture: six square faces side by side in one 2D texture (+X -X +Y -Y +Z -Z), read with n2_textureCube
        Texture3D, ///< a 3D texture: `depth` slices side by side in one 2D texture, read with n2_texture3D; the header gives "depth"
    };

    /// The parameter types that hold a Texture and take one of the five 2D texture slots (a cube and a 3D texture are atlases, stage 9e)
    [[nodiscard]] constexpr bool IsTextureParam(const ShaderParamType type)
    {
        return type == ShaderParamType::Texture || type == ShaderParamType::Cube || type == ShaderParamType::Texture3D;
    }

    /// One declared parameter. The reserved names bind to the standard Material fields: a `color` named baseColor takes the
    /// material's base colour (times the draw's tint) and a `texture` named baseColorTexture its base colour texture, unless the
    /// `.mat` sets them
    struct ShaderParam
    {
        std::string name;
        ShaderParamType type = ShaderParamType::Float;
        std::array<float, 4> defaultValue{};  ///< a non-texture default (a bool or int: the value cast)
        std::string defaultTexture = "white"; ///< a texture's default: white, black, normal or none
        int slot = -1;                        ///< a texture's slot 0-4 (the cook's rule: baseColorTexture first, then declaration order)
        int depth = 0;                        ///< a Texture3D's slice count (2 to 256)
        /// The editor's hints (ignored by the runtime)
        std::string label;
        std::optional<float> min, max;
    };

    /// The header's "state": hints that supply a material's defaults (a value the `.mat` sets wins). Empty when absent
    struct ShaderStateHints
    {
        std::optional<std::string> blend;      ///< none, alpha or additive
        std::optional<std::string> cull;       ///< back, front or none
        std::optional<bool> depthWrite;
        std::optional<std::string> queue;
    };

    /**
     * A user shader (`"Shader"`, a `.n2shader` file; orchestrator/proposals/user-shaders.md). The asset holds what the file's header
     * declares (name, parameters, whether it reads the scene lighting, state hints) and, when asked for a backend, the COOKED payload
     * for it: tools/n2shader writes it under `.import/shaders/<uuid>/` (manifest.json and one payload per target), and the engine
     * builds the Renderer::Common::UserShaderBlob a backend takes from it (MakeBlob). A shader with no cooked payload, a stale one
     * (its parameters differ from the header's), or none for the renderer's backend cannot be drawn: GpuCache then gives the Material
     * a fallback and logs why, once.
     *
     * Every successful Load bumps GetVersion, which a Material mixes into its GPU version, so a reloaded shader remakes the GPU
     * materials that use it on their next draw.
     */
    class Shader final : public Base::Asset
    {
    public:
        Shader() = default;

        /// A shader from the text of a `.n2shader` (the header, a line that is exactly `#stage fragment`, the GLSL). nullptr, with an
        /// error naming `debugName` and the line, when the header is not valid
        [[nodiscard]] static std::shared_ptr<Shader> FromSource(std::string_view text, std::string_view debugName = "shader");

        static void RegisterLoader();
        bool Load(const std::filesystem::path &path) override;
        static constexpr std::string_view ResourceTypeName = "Shader";
        static constexpr std::string_view Extension = ".n2shader";
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        [[nodiscard]] const std::string &GetName() const { return _name; }
        [[nodiscard]] const std::vector<ShaderParam> &GetParams() const { return _params; }
        [[nodiscard]] const ShaderParam *FindParam(std::string_view name) const;
        [[nodiscard]] bool UsesLighting() const { return _usesLighting; }
        /// Whether the file has a `#stage vertex` block (a custom vertex stage, stage 9d). Such a shader draws from its cooked payloads
        /// only: the runtime compile (the C++ twin of the cook) does not build vertex stages
        [[nodiscard]] bool HasVertexStage() const { return _hasVertexStage; }
        /// Whether the shader draws from cooked payloads only: a custom vertex stage or a cube or 3D texture parameter (their lookup
        /// functions are in the cook's prelude, which the runtime compile's C++ twin does not embed)
        [[nodiscard]] bool NeedsCook() const { return _hasVertexStage || _hasAtlasTexture; }
        [[nodiscard]] const ShaderStateHints &GetStateHints() const { return _state; }
        /// The file it was loaded from (empty for one made with FromSource)
        [[nodiscard]] const std::string &GetSourcePath() const { return _sourcePath; }
        /// Goes up by one on every Load; 1 for a new shader
        [[nodiscard]] std::uint64_t GetVersion() const { return _version; }

        /// Reads the cooked payload for `target` from `directory` (a folder holding manifest.json and the payloads) instead of from
        /// `.import/shaders/<uuid>/`: for tests and for a mod that ships its own. An empty path goes back to the default
        void SetCookedDirectory(std::filesystem::path directory);
        /// The folder the cooked payload is read from: the one SetCookedDirectory gave, else the project's `.import/shaders/<uuid>/`
        [[nodiscard]] std::filesystem::path GetCookedDirectory() const;

        /// The blob for `target`: the cooked manifest and that target's payload, checked against the header (the parameters must be
        /// the same names and types, in the same order). nullopt, with `error` set to why (not cooked, stale, no payload for the
        /// target, unreadable), otherwise. The blob is the backend's to validate (ValidateUserShaderBlob) before it touches a driver.
        /// MakeCookedBlob reads only the cooked data; this one then, for Vulkan with RenderSettings::GetRuntimeShaderCompile() and a
        /// linked glslang, or for OpenGL, builds the payload from the .n2shader's own text (UserShaderCompiler), through the shader
        /// cache when RenderSettings has a folder for it. A pack whose sources were stripped has no text to build from
        [[nodiscard]] std::optional<Renderer::Common::UserShaderBlob> MakeBlob(Renderer::Common::UserShaderTarget target,
                                                                               std::string &error) const;
        [[nodiscard]] std::optional<Renderer::Common::UserShaderBlob> MakeCookedBlob(Renderer::Common::UserShaderTarget target,
                                                                                     std::string &error) const;

        /// The file's text as it was loaded: LF line ends, no byte order mark (empty for a shader that failed to load)
        [[nodiscard]] const std::string &GetFileText() const { return _text; }
        /// The fragment stage (every line after the exact `#stage fragment` line, joined with LF) and the file line number of its first
        /// line, as the cook's parser reads them. False when there is no such line
        [[nodiscard]] bool GetStage(std::string &source, int &firstLine) const;

    private:
        bool Parse(std::string_view text, std::string_view debugName);
        using PayloadReader = std::function<std::expected<std::vector<std::uint8_t>, std::string>(const std::string &file)>;
        /// The blob from a manifest and the payload file it names (read through `readPayload`), checked against the header
        [[nodiscard]] std::optional<Renderer::Common::UserShaderBlob> BlobFromManifest(const nlohmann::json &manifest,
                                                                                       const PayloadReader &readPayload,
                                                                                       Renderer::Common::UserShaderTarget target,
                                                                                       std::string &error) const;
        /// Compiles (or reads from the shader cache) the payload of `target` from the file's text; nullopt, with `why`, when it can't
        [[nodiscard]] std::optional<Renderer::Common::UserShaderBlob> MakeRuntimeBlob(Renderer::Common::UserShaderTarget target,
                                                                                      std::string &why) const;
        std::string _text;

        std::string _name;
        std::vector<ShaderParam> _params;
        bool _usesLighting = false;
        bool _hasVertexStage = false;
        bool _hasAtlasTexture = false;
        ShaderStateHints _state;
        std::string _sourcePath;
        std::uint64_t _version = 1;
        std::filesystem::path _cookedDirectory;
    };

    /// The loader registered for .n2shader
    std::shared_ptr<Base::Asset> LoadShaderFromFile(const std::filesystem::path &path);
}
