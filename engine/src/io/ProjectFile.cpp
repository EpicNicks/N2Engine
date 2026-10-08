#include "engine/io/ProjectFile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "engine/Version.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/scripting/LuaScriptTemplate.hpp"

namespace N2Engine::IO
{
    namespace
    {
        constexpr std::array<std::string_view, 7> KnownKeys = {
            "formatVersion", "name", "projectId", "engineVersion", "startupScene", "scenes", "settings",
        };

        constexpr std::string_view ScriptsFolder = "scripts";
        constexpr std::string_view ExampleScript = "Example.lua";

        bool IsKnownKey(const std::string &key)
        {
            return std::ranges::find(KnownKeys, key) != KnownKeys.end();
        }

        bool EndsWithIgnoringCase(const std::string_view text, const std::string_view suffix)
        {
            if (text.size() < suffix.size())
            {
                return false;
            }
            const std::string_view tail = text.substr(text.size() - suffix.size());
            return std::ranges::equal(tail, suffix, [](const char a, const char b)
            {
                const auto lower = [](const char c)
                {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                };
                return lower(a) == lower(b);
            });
        }

        struct VersionNumber
        {
            uint32_t major = 0;
            uint32_t minor = 0;
            uint32_t patch = 0;
        };

        std::optional<VersionNumber> ParseVersion(const std::string_view text)
        {
            VersionNumber version;
            uint32_t *parts[] = {&version.major, &version.minor, &version.patch};
            const char *position = text.data();
            const char *const end = text.data() + text.size();
            for (size_t i = 0; i < 3; ++i)
            {
                if (i > 0)
                {
                    if (position == end || *position != '.')
                    {
                        return std::nullopt;
                    }
                    ++position;
                }
                if (position == end || *position < '0' || *position > '9')
                {
                    return std::nullopt;
                }
                const auto [next, error] = std::from_chars(position, end, *parts[i]);
                if (error != std::errc{})
                {
                    return std::nullopt;
                }
                position = next;
            }
            if (position != end)
            {
                return std::nullopt;
            }
            return version;
        }

        std::string ReadWholeFile(const std::filesystem::path &path, bool &ok)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                ok = false;
                return {};
            }
            std::ostringstream text;
            text << file.rdbuf();
            ok = !file.bad();
            return std::move(text).str();
        }

        std::string EmptySceneText(const std::string &sceneName)
        {
            // What Scene::Serialize writes for a scene with no objects
            const nlohmann::json scene = {{"name", sceneName}, {"rootGameObjects", nlohmann::json::array()}};
            return scene.dump(2) + "\n";
        }

        /// Retries of a rename that failed for a moment (WriteTextFileAtomically)
        constexpr int RenameRetries = 10;

        uint32_t RandomTag()
        {
            static thread_local std::mt19937 generator{std::random_device{}()};
            return static_cast<uint32_t>(generator());
        }

        /// std::ios::noreplace (C++23: fail if the file exists) where the library has it
        std::ios::openmode NoReplaceFlag()
        {
#if defined(__cpp_lib_ios_noreplace)
            return std::ios::noreplace;
#else
            return std::ios::openmode{};
#endif
        }

        /// The errors another process holding the file open gives a rename, which go away on their own
        bool IsTransientRenameError(const std::error_code &error)
        {
#ifdef _WIN32
            // ERROR_ACCESS_DENIED (5), ERROR_SHARING_VIOLATION (32), ERROR_LOCK_VIOLATION (33)
            if (error.category() == std::system_category() &&
                (error.value() == 5 || error.value() == 32 || error.value() == 33))
            {
                return true;
            }
#endif
            return error == std::errc::permission_denied || error == std::errc::device_or_resource_busy;
        }

        std::string PathText(const std::filesystem::path &path)
        {
            // u8string: a path the narrow code page can't spell must not throw while describing an error
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }
    }

    std::filesystem::path ProjectFile::PathIn(const std::filesystem::path &projectDir)
    {
        return projectDir / FileName;
    }

    std::expected<ProjectFile, std::string> ProjectFile::FromJson(const nlohmann::json &j)
    {
        if (!j.is_object())
        {
            return std::unexpected("a project file must hold a JSON object");
        }

        ProjectFile project;
        project.unknownKeys = nlohmann::json::object();
        for (const auto &[key, value] : j.items())
        {
            if (!IsKnownKey(key))
            {
                project.unknownKeys[key] = value;
            }
        }

        const auto formatVersion = j.find("formatVersion");
        if (formatVersion == j.end() || !formatVersion->is_number_integer())
        {
            return std::unexpected("formatVersion: missing or not an integer");
        }
        // Range-checked as int64 first, so a huge number can't wrap into range
        const int64_t format = formatVersion->get<int64_t>();
        if (format < 1 || format > CurrentFormatVersion)
        {
            return std::unexpected(format > CurrentFormatVersion
                                       ? std::format("formatVersion: {} is newer than this engine reads ({}); "
                                                     "open it with a newer engine", format, CurrentFormatVersion)
                                       : std::format("formatVersion: {} is not a format version", format));
        }
        project.formatVersion = static_cast<int>(format);

        const auto name = j.find("name");
        if (name == j.end() || !name->is_string())
        {
            return std::unexpected("name: missing or not a string");
        }
        project.name = name->get<std::string>();

        const auto projectId = j.find("projectId");
        if (projectId == j.end() || !projectId->is_string())
        {
            return std::unexpected("projectId: missing or not a string");
        }
        const auto id = Math::UUID::FromString(projectId->get<std::string>());
        if (!id)
        {
            return std::unexpected("projectId: '" + projectId->get<std::string>() + "' is not a UUID");
        }
        project.projectId = *id;

        const auto engineVersion = j.find("engineVersion");
        if (engineVersion == j.end() || !engineVersion->is_string())
        {
            return std::unexpected("engineVersion: missing or not a string");
        }
        project.engineVersion = engineVersion->get<std::string>();

        if (const auto startupScene = j.find("startupScene"); startupScene != j.end() && !startupScene->is_null())
        {
            if (!startupScene->is_string())
            {
                return std::unexpected("startupScene: not a string");
            }
            project.startupScene = startupScene->get<std::string>();
        }

        if (const auto scenes = j.find("scenes"); scenes != j.end() && !scenes->is_null())
        {
            if (!scenes->is_array())
            {
                return std::unexpected("scenes: not an array");
            }
            for (const nlohmann::json &scene : *scenes)
            {
                if (!scene.is_string())
                {
                    return std::unexpected("scenes: an entry is not a string");
                }
                project.scenes.push_back(scene.get<std::string>());
            }
        }

        if (const auto settings = j.find("settings"); settings != j.end() && !settings->is_null())
        {
            if (!settings->is_object())
            {
                return std::unexpected("settings: not an object");
            }
            project.settings = *settings;
        }

        if (auto valid = project.Validate(); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        return project;
    }

    std::expected<ProjectFile, std::string> ProjectFile::Parse(const std::string_view text)
    {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded())
        {
            return std::unexpected("not valid JSON");
        }
        return FromJson(j);
    }

    std::expected<ProjectFile, std::string> ProjectFile::Load(const std::filesystem::path &projectDir)
    {
        const std::filesystem::path path = PathIn(projectDir);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
        {
            return std::unexpected(std::format("{} not found: {} is not a project (create one with "
                                               "N2EditorHost --create)", FileName, PathText(projectDir)));
        }
        bool ok = true;
        const std::string text = ReadWholeFile(path, ok);
        if (!ok)
        {
            return std::unexpected(std::format("can't read {}", PathText(path)));
        }
        auto project = Parse(text);
        if (!project)
        {
            return std::unexpected(std::format("{}: {}", PathText(path), project.error()));
        }
        return project;
    }

    std::expected<void, std::string> ProjectFile::Validate() const
    {
        if (formatVersion < 1 || formatVersion > CurrentFormatVersion)
        {
            return std::unexpected(std::format("formatVersion: {} is not between 1 and {}", formatVersion,
                                               CurrentFormatVersion));
        }
        if (name.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            return std::unexpected("name: empty");
        }
        if (projectId == Math::UUID::ZERO)
        {
            return std::unexpected("projectId: the zero UUID can't be a project's id");
        }
        if (!IsEngineVersion(engineVersion))
        {
            return std::unexpected("engineVersion: '" + engineVersion + "' is not major.minor.patch");
        }
        if (!startupScene.empty() && !IsScenePath(startupScene))
        {
            return std::unexpected("startupScene: '" + startupScene + "' is not a res:// path to a .scene file");
        }
        for (const std::string &scene : scenes)
        {
            if (!IsScenePath(scene))
            {
                return std::unexpected("scenes: '" + scene + "' is not a res:// path to a .scene file");
            }
        }
        if (!settings.is_object())
        {
            return std::unexpected("settings: not an object");
        }
        if (!unknownKeys.is_object())
        {
            return std::unexpected("unknownKeys: not an object");
        }
        return {};
    }

    nlohmann::json ProjectFile::ToJson() const
    {
        nlohmann::json j = unknownKeys.is_object() ? unknownKeys : nlohmann::json::object();
        // A known key in unknownKeys (set by hand) must not survive under the real value
        for (const std::string_view key : KnownKeys)
        {
            j.erase(std::string(key));
        }
        j["formatVersion"] = formatVersion;
        j["name"] = name;
        j["projectId"] = projectId.ToString();
        j["engineVersion"] = engineVersion;
        j["startupScene"] = startupScene;
        j["scenes"] = scenes;
        j["settings"] = settings;
        return j;
    }

    std::string ProjectFile::ToText() const
    {
        return ToJson().dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    }

    std::expected<void, std::string> ProjectFile::Save(const std::filesystem::path &projectDir) const
    {
        if (auto valid = Validate(); !valid)
        {
            return std::unexpected("not saved: " + valid.error());
        }
        return WriteTextFileAtomically(PathIn(projectDir), ToText());
    }

    bool ProjectFile::IsScenePath(const std::string_view text)
    {
        if (!text.starts_with("res://") || text.find('\0') != std::string_view::npos)
        {
            return false;
        }
        const ResourcePath path{std::string(text)};
        const std::string &relative = path.GetPath();
        if (path.GetType() != PathType::Resource || relative.empty() || relative == ".." || relative.starts_with("../"))
        {
            return false;
        }
        // Not a root name, a rooted path or a ':' anywhere: on NTFS "name.scene:x" names an alternate data stream
        const std::filesystem::path asPath = PathFromUtf8(relative);
        if (relative.find(':') != std::string::npos || asPath.has_root_name() || asPath.has_root_directory() ||
            asPath.is_absolute())
        {
            return false;
        }
        const std::string fileName = PathToUtf8(asPath.filename());
        return fileName.size() > SceneExtension.size() && EndsWithIgnoringCase(fileName, SceneExtension);
    }

    std::filesystem::path ProjectFile::UserDataBase()
    {
#ifdef _WIN32
        char *appData = nullptr;
        size_t length = 0;
        if (_dupenv_s(&appData, &length, "APPDATA") == 0 && appData != nullptr)
        {
            std::filesystem::path path(appData);
            std::free(appData);
            return path / "N2Engine";
        }
        return std::filesystem::path(".");
#else
        if (const char *home = std::getenv("HOME"); home != nullptr)
        {
            return std::filesystem::path(home) / ".n2engine";
        }
        return std::filesystem::path(".");
#endif
    }

    std::string ProjectFile::SanitizeFolderName(const std::string_view name)
    {
        constexpr size_t MaxLength = 48;
        std::string folder;
        folder.reserve(std::min(name.size(), MaxLength));
        for (const char c : name)
        {
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '-' || c == '_' || c == '.';
            folder += keep ? c : '_';
        }
        // Windows drops a trailing '.', and a leading one hides the folder elsewhere; '_' runs at the ends are noise
        const size_t first = folder.find_first_not_of("._");
        if (first == std::string::npos)
        {
            return "Project";
        }
        folder = folder.substr(first, folder.find_last_not_of("._") - first + 1);
        if (folder.size() > MaxLength)
        {
            folder.resize(MaxLength);
            folder.erase(folder.find_last_not_of("._") + 1);
        }
        return folder;
    }

    std::filesystem::path ProjectFile::UserDataPath(const std::filesystem::path &base) const
    {
        const std::string id = projectId.ToString();
        return (base.empty() ? UserDataBase() : base) / (SanitizeFolderName(name) + "-" + id.substr(0, 8));
    }

    bool IsEngineVersion(const std::string_view text)
    {
        return ParseVersion(text).has_value();
    }

    EngineVersionMatch CompareEngineVersions(const std::string_view projectVersion,
                                             const std::string_view engineVersion)
    {
        const auto project = ParseVersion(projectVersion);
        const auto engine = ParseVersion(engineVersion);
        if (!project || !engine)
        {
            return EngineVersionMatch::Invalid;
        }
        if (project->major != engine->major)
        {
            return EngineVersionMatch::MajorDiffers;
        }
        return project->minor == engine->minor ? EngineVersionMatch::Compatible : EngineVersionMatch::MinorDiffers;
    }

    std::expected<void, std::string> WriteTextFileAtomically(const std::filesystem::path &path,
                                                             const std::string_view text)
    {
        // A temporary file of its own beside the target (the rename must stay on one volume), created exclusively,
        // so two writers, or a stale file from a crash, never share one
        std::filesystem::path temporary;
        std::ofstream file;
        for (int attempt = 0; attempt < 16 && !file.is_open(); ++attempt)
        {
            temporary = path;
            temporary += std::format(".{:08x}.tmp", RandomTag());
            file.open(temporary, std::ios::binary | std::ios::out | NoReplaceFlag());
        }
        if (!file.is_open())
        {
            return std::unexpected(std::format("can't create a temporary file beside {}", PathText(path)));
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        // To the operating system, not to the disk: this protects against a crash or a failed write of this process,
        // not against losing power (that would need FlushFileBuffers/fsync)
        file.flush();
        const bool written = static_cast<bool>(file);
        file.close();
        std::error_code ignored;
        if (!written || file.fail())
        {
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(std::format("can't write {} (is the disk full?)", PathText(temporary)));
        }

        // Replaces path when it exists (MoveFileExW with MOVEFILE_REPLACE_EXISTING on Windows, rename on POSIX). On
        // Windows another process reading the file (an editor, a virus scanner, the search indexer) makes the rename
        // fail for a moment, so that is retried briefly.
        std::error_code error;
        for (int attempt = 0;; ++attempt)
        {
            error.clear();
            std::filesystem::rename(temporary, path, error);
            if (!error || attempt >= RenameRetries || !IsTransientRenameError(error))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10 * (attempt + 1), 50)));
        }
        if (error)
        {
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(std::format("can't replace {}: {}", PathText(path), error.message()));
        }
        return {};
    }

    std::expected<ProjectFile, CreateProjectError> CreateProject(const std::filesystem::path &projectDir,
                                                                 const CreateProjectOptions &options)
    {
        const auto fail = [](std::string message)
        {
            return std::unexpected(CreateProjectError{CreateProjectErrorKind::Failed, std::move(message)});
        };

        if (projectDir.empty())
        {
            return fail("no project folder given");
        }
        std::error_code error;
        if (std::filesystem::exists(ProjectFile::PathIn(projectDir), error))
        {
            return std::unexpected(CreateProjectError{
                CreateProjectErrorKind::AlreadyAProject,
                std::format("{} already has a {}; nothing was changed", PathText(projectDir), ProjectFile::FileName)});
        }
        if (std::filesystem::exists(projectDir, error) && !std::filesystem::is_directory(projectDir, error))
        {
            return fail(std::format("{} exists and is not a folder", PathText(projectDir)));
        }

        ProjectFile project;
        project.name = options.name;
        if (project.name.empty())
        {
            // The folder's own name: "C:/games/My Game/" names "My Game"
            const std::filesystem::path absolute = std::filesystem::absolute(projectDir, error).lexically_normal();
            std::filesystem::path folder = absolute.filename();
            if (folder.empty())
            {
                folder = absolute.parent_path().filename();
            }
            const std::u8string folderName = folder.u8string();
            project.name.assign(folderName.begin(), folderName.end());
            if (project.name.empty())
            {
                project.name = "Project";
            }
        }
        project.projectId = options.projectId.value_or(Math::UUID::Random());
        project.engineVersion = options.engineVersion.empty() ? std::string(EngineVersion()) : options.engineVersion;
        project.startupScene = std::string(DefaultStartupScene);
        project.scenes = {project.startupScene};

        // Checked before anything is created, so bad options leave no folders behind
        if (auto valid = project.Validate(); !valid)
        {
            return fail(valid.error());
        }

        const std::filesystem::path assets = projectDir / "assets";
        const std::filesystem::path sceneFile =
            assets / std::filesystem::path(std::string(DefaultStartupScene.substr(std::string_view("res://").size())));
        const std::filesystem::path scriptsFolder = assets / ScriptsFolder;
        std::filesystem::create_directories(sceneFile.parent_path(), error);
        if (!error)
        {
            std::filesystem::create_directories(scriptsFolder, error);
        }
        if (error)
        {
            return fail(std::format("can't create the folders of {}: {}", PathText(projectDir), error.message()));
        }

        // Kept when they exist (adopting a folder): only missing files are written
        struct NewFile
        {
            std::filesystem::path path;
            std::string text;
        };
        const NewFile files[] = {
            {sceneFile, EmptySceneText(sceneFile.stem().string())},
            {scriptsFolder / ExampleScript, Scripting::MakeLuaScriptTemplate(ExampleScript)},
            {projectDir / ".gitignore", "# The editor's per-machine state (caches, logs, play-mode scenes)\n.n2/\n"},
        };
        for (const NewFile &file : files)
        {
            if (std::filesystem::exists(file.path, error))
            {
                continue;
            }
            if (auto written = WriteTextFileAtomically(file.path, file.text); !written)
            {
                return fail(written.error());
            }
        }

        // Last: a folder is a project once this exists, so a failure above leaves no half-made project
        if (auto saved = project.Save(projectDir); !saved)
        {
            return fail(saved.error());
        }
        return project;
    }
}
