#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/UUID.hpp>

namespace N2Engine::IO
{
    /**
     * A project's project.n2proj, at the project's root folder (next to assets/ and .import/): JSON with
     *
     *     formatVersion  1 (a file of a newer format is refused)
     *     name           the project's name, not empty
     *     projectId      a UUID, made once when the project is created: the namespace of its asset UUIDs
     *                    (ResourceUUID::Initialize), so they don't depend on where the folder is
     *     engineVersion  the engine version that created it, "major.minor.patch" (CompareEngineVersions)
     *     startupScene   the scene a host opens first: a res:// path to a .scene file, or "" (optional)
     *     scenes         the project's scene list, res:// paths to .scene files (optional)
     *     settings       one block per subsystem ("layers", "input", "physics", ...), each owned by that subsystem's
     *                    own Serialize/Deserialize (ApplyProjectSettings); ProjectFile doesn't read them (optional)
     *
     * CPU only: parsing, validating, saving and creating touch nothing but files, so tools and tests use it without
     * initialising the engine. Keys this version doesn't know are kept (unknownKeys, and anything inside settings) and
     * written back by Save, so a file edited by a newer editor survives an older one.
     */
    struct ProjectFile
    {
        static constexpr std::string_view FileName = "project.n2proj";
        static constexpr int CurrentFormatVersion = 1;
        /// The extension of a scene file (compared case-insensitively)
        static constexpr std::string_view SceneExtension = ".scene";

        int formatVersion = CurrentFormatVersion;
        std::string name;
        Math::UUID projectId = Math::UUID::ZERO;
        std::string engineVersion;
        std::string startupScene;
        std::vector<std::string> scenes;
        nlohmann::json settings = nlohmann::json::object();
        /// Top-level keys this version doesn't know, as they were read; ToJson writes them back
        nlohmann::json unknownKeys = nlohmann::json::object();

        /// <projectDir>/project.n2proj
        [[nodiscard]] static std::filesystem::path PathIn(const std::filesystem::path &projectDir);

        /// The file's values, checked (Validate). The error names the key at fault.
        [[nodiscard]] static std::expected<ProjectFile, std::string> FromJson(const nlohmann::json &j);
        /// FromJson of JSON text; invalid JSON is an error too
        [[nodiscard]] static std::expected<ProjectFile, std::string> Parse(std::string_view text);
        /// Reads <projectDir>/project.n2proj; an error when it is missing, unreadable or invalid
        [[nodiscard]] static std::expected<ProjectFile, std::string> Load(const std::filesystem::path &projectDir);

        /// Every rule above (FromJson applies it to what it read; Save to what it writes): an error naming the key
        [[nodiscard]] std::expected<void, std::string> Validate() const;

        /// The file's JSON: the known keys over unknownKeys. projectId in lower case.
        [[nodiscard]] nlohmann::json ToJson() const;
        /// ToJson as the file holds it: indented by 2, keys sorted, ending in a newline
        [[nodiscard]] std::string ToText() const;
        /// Validates, then writes <projectDir>/project.n2proj (WriteTextFileAtomically). The folder must exist.
        [[nodiscard]] std::expected<void, std::string> Save(const std::filesystem::path &projectDir) const;

        /**
         * Whether text is a scene path a project file may name: "res://" then a path inside the assets folder (not
         * empty, not leaving it with "..") whose file name ends in ".scene" (any case)
         */
        [[nodiscard]] static bool IsScenePath(std::string_view text);

        /**
         * Where this project's user:// files go: <base>/<sanitised name>-<first 8 hex digits of projectId>, so two
         * projects never share saves and moving the project's folder keeps them (renaming the project starts a new
         * folder; the old one is left as it is). base is UserDataBase() unless given (empty).
         */
        [[nodiscard]] std::filesystem::path UserDataPath(const std::filesystem::path &base = {}) const;
        /// %APPDATA%/N2Engine on Windows; elsewhere $XDG_DATA_HOME/n2engine, else $HOME/.local/share/n2engine (an empty
        /// or relative value counts as unset). "." when none of them is usable (a service or container with no HOME):
        /// the folder every project's user data folder is in, and what user:// meant before projects had their own
        [[nodiscard]] static std::filesystem::path UserDataBase();
        /// A project name made safe as one folder name: letters, digits, '-', '_' and '.' kept, anything else
        /// (spaces, separators, non-ASCII) becomes '_', leading and trailing '.' and '_' dropped, at most 48
        /// characters; "Project" when nothing is left
        [[nodiscard]] static std::string SanitizeFolderName(std::string_view name);
    };

    /// How a project's engineVersion compares with the running engine's
    enum class EngineVersionMatch
    {
        /// The same major and minor version (the patch may differ)
        Compatible,
        /// The same major version, another minor one: a warning
        MinorDiffers,
        /// Another major version: the project can't be opened
        MajorDiffers,
        /// Either isn't "major.minor.patch"
        Invalid,
    };

    [[nodiscard]] EngineVersionMatch CompareEngineVersions(std::string_view projectVersion,
                                                           std::string_view engineVersion);

    /// "major.minor.patch", each part decimal digits only
    [[nodiscard]] bool IsEngineVersion(std::string_view text);

    /**
     * Writes text to path through a temporary file next to it (<path>.tmp), renamed over path once complete, so a
     * crash or a full disk never leaves a half-written file where the old one was. An error describing the failure.
     */
    [[nodiscard]] std::expected<void, std::string> WriteTextFileAtomically(const std::filesystem::path &path,
                                                                           std::string_view text);

    /// What CreateProject makes
    struct CreateProjectOptions
    {
        /// The project's name; empty: the folder's name ("Project" for a folder without one)
        std::string name;
        /// The project's id; nullopt: a new random one. Adopting an existing folder whose assets are referenced by
        /// UUID, pass the namespace they were made in (ResourceUUID::NamespaceForProjectDir for a folder opened
        /// before projects had ids), so every UUID stays the same.
        std::optional<Math::UUID> projectId;
        /// The engine version the file records; empty: EngineVersion()
        std::string engineVersion;
    };

    /// The res:// path CreateProject's first scene has, and the project's startup scene
    inline constexpr std::string_view DefaultStartupScene = "res://scenes/Main.scene";

    enum class CreateProjectErrorKind
    {
        /// projectDir already has a project.n2proj; nothing was written
        AlreadyAProject,
        /// Bad options (an empty or zero id, a name of only whitespace...) or a file that couldn't be written
        Failed,
    };

    struct CreateProjectError
    {
        CreateProjectErrorKind kind = CreateProjectErrorKind::Failed;
        std::string message;
    };

    /**
     * Makes projectDir a project, creating the folder (and its parents) if needed: project.n2proj, assets/scenes/
     * with an empty Main.scene (DefaultStartupScene, the startup scene and only entry of scenes), assets/scripts/
     * with Example.lua (a script in the engine's script model, as the editor's CreateScript writes), and a
     * .gitignore holding ".n2/" (the editor's per-machine folder). A file that already exists is kept, except that
     * an existing project.n2proj is an error (CreateProjectErrorKind::AlreadyAProject) and nothing is written. CPU only:
     * no engine subsystem is touched. Returns the project file written.
     */
    [[nodiscard]] std::expected<ProjectFile, CreateProjectError> CreateProject(const std::filesystem::path &projectDir,
                                                                               const CreateProjectOptions &options = {});
}
