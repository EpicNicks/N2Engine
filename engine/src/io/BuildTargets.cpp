#include "engine/io/BuildTargets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace N2Engine::IO::BuildTargets
{
    namespace
    {
        using nlohmann::json;

        /// A key or value quoted in an error: printable ASCII only, at most 64 characters, so a hostile key cannot put
        /// control characters or a wall of text into a log or a client's dialog
        std::string Quote(const std::string_view text)
        {
            std::string out;
            for (const char c : text.substr(0, 64))
            {
                const auto u = static_cast<unsigned char>(c);
                out += (u >= 0x20 && u < 0x7f) ? c : '?';
            }
            if (text.size() > 64)
            {
                out += "...";
            }
            return out;
        }

        std::string Lower(const std::string_view text)
        {
            std::string out;
            for (const char c : text)
            {
                if (c != '_' && c != '-' && c != ' ' && c != '.')
                {
                    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            return out;
        }

        /// A key whose name says it holds signing material or credentials (case, '_', '-', '.' and spaces ignored)
        bool LooksLikeSecretKey(const std::string_view key)
        {
            static constexpr std::array<std::string_view, 11> words = {
                "password", "passwd", "passphrase", "secret", "token", "privatekey", "credential", "apikey",
                "storepass", "keypass", "certificatedata"};
            const std::string lowered = Lower(key);
            return std::ranges::any_of(words, [&](const std::string_view word) { return lowered.find(word) != std::string::npos; });
        }

        /// Any key at any depth that looks like a secret, or any string holding a PEM block: the path to it, else ""
        std::string FindSecret(const json &value, const std::string &path)
        {
            if (value.is_object())
            {
                for (const auto &[key, child] : value.items())
                {
                    const std::string childPath = path + "." + Quote(key);
                    if (LooksLikeSecretKey(key))
                    {
                        return childPath;
                    }
                    if (std::string found = FindSecret(child, childPath); !found.empty())
                    {
                        return found;
                    }
                }
            }
            else if (value.is_array())
            {
                for (std::size_t i = 0; i < value.size(); ++i)
                {
                    if (std::string found = FindSecret(value[i], std::format("{}[{}]", path, i)); !found.empty())
                    {
                        return found;
                    }
                }
            }
            else if (value.is_string() && value.get_ref<const std::string &>().find("-----BEGIN") != std::string::npos)
            {
                return path;
            }
            return {};
        }

        bool IsLetter(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
        bool IsDigit(const char c) { return c >= '0' && c <= '9'; }

        bool IsAppId(const std::string_view text)
        {
            if (text.size() < 3 || text.size() > 150)
            {
                return false;
            }
            int segments = 0;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t end = std::min(text.find('.', start), text.size());
                const std::string_view segment = text.substr(start, end - start);
                if (segment.empty() || !IsLetter(segment.front())
                    || !std::ranges::all_of(segment, [](const char c) { return IsLetter(c) || IsDigit(c) || c == '_'; }))
                {
                    return false;
                }
                ++segments;
                start = end + 1;
            }
            return segments >= 2;
        }

        bool IsVersion(const std::string_view text)
        {
            int parts = 0;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t end = std::min(text.find('.', start), text.size());
                const std::string_view part = text.substr(start, end - start);
                if (part.empty() || part.size() > 5 || !std::ranges::all_of(part, IsDigit))
                {
                    return false;
                }
                ++parts;
                start = end + 1;
            }
            return parts == 3;
        }

        bool IsDisplayName(const std::string_view text)
        {
            if (text.empty() || text.size() > 256)
            {
                return false;
            }
            std::size_t characters = 0;
            for (const char c : text)
            {
                const auto u = static_cast<unsigned char>(c);
                if (u < 0x20 || u == 0x7f)
                {
                    return false;
                }
                // Count code points: every byte but a UTF-8 continuation byte starts one
                if ((u & 0xC0) != 0x80)
                {
                    ++characters;
                }
            }
            return characters <= 64;
        }

        /// res://, then a path inside the assets folder (no empty or ".." segment, no backslash) ending in .png (any case)
        bool IsIconPath(const std::string_view text)
        {
            constexpr std::string_view prefix = "res://";
            if (!text.starts_with(prefix) || text.size() > 512 || text.find('\\') != std::string_view::npos)
            {
                return false;
            }
            const std::string_view rest = text.substr(prefix.size());
            std::size_t start = 0;
            while (start <= rest.size())
            {
                const std::size_t end = std::min(rest.find('/', start), rest.size());
                const std::string_view segment = rest.substr(start, end - start);
                if (segment.empty() || segment == "..")
                {
                    return false;
                }
                start = end + 1;
            }
            // A file name before the extension, and .png in any case
            const std::size_t lastSlash = rest.rfind('/');
            const std::string_view fileName = lastSlash == std::string_view::npos ? rest : rest.substr(lastSlash + 1);
            if (fileName.size() <= 4)
            {
                return false;
            }
            std::string extension(fileName.substr(fileName.size() - 4));
            for (char &c : extension)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return extension == ".png";
        }

        bool IsProfileName(const std::string_view text)
        {
            if (text.empty() || text.size() > 64 || !(IsLetter(text.front()) || IsDigit(text.front())))
            {
                return false;
            }
            return std::ranges::all_of(text, [](const char c) { return IsLetter(c) || IsDigit(c) || c == '.' || c == '_' || c == '-'; });
        }

        bool OneOf(const json &value, const std::initializer_list<std::string_view> allowed)
        {
            return value.is_string()
                   && std::ranges::find(allowed, std::string_view(value.get_ref<const std::string &>())) != allowed.end();
        }

        std::expected<void, std::string> ValidateModules(const json &modules, const std::string &path)
        {
            if (!modules.is_object())
            {
                return std::unexpected(path + ": must be an object (the same keys as build.modules)");
            }
            for (const auto &[key, value] : modules.items())
            {
                if (key == "physics3d")
                {
                    if (!OneOf(value, {"none", "physx", "jolt", "both"}))
                    {
                        return std::unexpected(path + ".physics3d: must be \"none\", \"physx\", \"jolt\" or \"both\"");
                    }
                }
                else if (key == "physics2d" || key == "particles" || key == "softbodies" || key == "audio" || key == "ui")
                {
                    if (!value.is_boolean())
                    {
                        return std::unexpected(std::format("{}.{}: must be true or false", path, key));
                    }
                }
                else
                {
                    return std::unexpected(std::format("{}: unknown module \"{}\" (known: physics3d, physics2d, particles, "
                                                       "softbodies, audio, ui)", path, Quote(key)));
                }
            }
            return {};
        }

        std::expected<void, std::string> ValidateTarget(const std::string &name, const json &target)
        {
            const std::string path = "build.targets." + name;
            if (!target.is_object())
            {
                return std::unexpected(path + ": must be an object of that target's settings");
            }
            if (const std::string secret = FindSecret(target, path); !secret.empty())
            {
                return std::unexpected(secret + ": looks like a password, key or other secret. Project files are "
                                                "committed and shared: keep signing material in your own n2build "
                                                "settings and name its profile in signing.profile");
            }
            for (const auto &[key, value] : target.items())
            {
                const std::string at = path + "." + key;
                const auto fail = [&](const std::string_view rule) { return std::unexpected(std::format("{}: {}", at, rule)); };
                if (key == "enabled")
                {
                    if (!value.is_boolean()) return fail("must be true or false");
                }
                else if (key == "displayName")
                {
                    if (!value.is_string() || !IsDisplayName(value.get_ref<const std::string &>()))
                        return fail("must be 1 to 64 characters, with no control characters");
                }
                else if (key == "appId")
                {
                    if (!value.is_string() || !IsAppId(value.get_ref<const std::string &>()))
                        return fail("must be a reverse-domain id such as com.example.game: two or more segments of "
                                    "letters, digits and '_', each starting with a letter, at most 150 characters");
                }
                else if (key == "version")
                {
                    if (!value.is_string() || !IsVersion(value.get_ref<const std::string &>()))
                        return fail("must be \"major.minor.patch\", each part 1 to 5 digits");
                }
                else if (key == "buildNumber")
                {
                    if (!value.is_number_integer() || value.get<long long>() < 1 || value.get<long long>() > MaxBuildNumber)
                        return fail(std::format("must be a whole number from 1 to {}", MaxBuildNumber));
                }
                else if (key == "orientation")
                {
                    if (!OneOf(value, {"any", "landscape", "portrait"}))
                        return fail("must be \"any\", \"landscape\" or \"portrait\"");
                }
                else if (key == "renderer")
                {
                    if (!OneOf(value, {"auto", "software", "opengl", "vulkan", "webgpu"}))
                        return fail("must be \"auto\", \"software\", \"opengl\", \"vulkan\" or \"webgpu\"");
                }
                else if (key == "config")
                {
                    if (!OneOf(value, {"debug", "release"})) return fail("must be \"debug\" or \"release\"");
                }
                else if (key == "icon")
                {
                    if (!value.is_string() || !IsIconPath(value.get_ref<const std::string &>()))
                        return fail("must be a res:// path to a .png file inside the assets folder");
                }
                else if (key == "modules")
                {
                    if (auto checked = ValidateModules(value, at); !checked) return checked;
                }
                else if (key == "signing")
                {
                    if (!value.is_object())
                        return fail("must be an object: {\"profile\": \"<name of a profile in your n2build settings>\"}");
                    for (const auto &[signingKey, signingValue] : value.items())
                    {
                        if (signingKey != "profile")
                        {
                            return std::unexpected(std::format(
                                "{}.{}: signing holds only a profile name; keystores, certificates and passwords stay in "
                                "your own n2build settings", at, Quote(signingKey)));
                        }
                        if (!signingValue.is_string() || !IsProfileName(signingValue.get_ref<const std::string &>()))
                        {
                            return std::unexpected(at + ".profile: must be a profile name: letters, digits, '.', '_' and '-', "
                                                        "1 to 64 characters");
                        }
                    }
                }
                // Any other key is kept as it is (a newer editor's setting); FindSecret has already checked it
            }
            return {};
        }
    }

    bool IsTargetName(const std::string_view text)
    {
        if (text.empty() || text.size() > 32 || !(text.front() >= 'a' && text.front() <= 'z'))
        {
            return false;
        }
        return std::ranges::all_of(text, [](const char c) { return (c >= 'a' && c <= 'z') || IsDigit(c) || c == '-'; });
    }

    std::expected<void, std::string> Validate(const json &targets)
    {
        if (!targets.is_object())
        {
            return std::unexpected("build.targets: must be an object keyed by target name");
        }
        if (targets.size() > MaxTargets)
        {
            return std::unexpected(std::format("build.targets: at most {} targets", MaxTargets));
        }
        if (targets.dump().size() > MaxBytes)
        {
            return std::unexpected(std::format("build.targets: at most {} KiB of settings", MaxBytes / 1024));
        }
        for (const auto &[name, target] : targets.items())
        {
            if (!IsTargetName(name))
            {
                return std::unexpected(std::format("build.targets: \"{}\" is not a target name (a lowercase letter, then "
                                                   "lowercase letters, digits and '-', at most 32)", Quote(name)));
            }
            if (auto checked = ValidateTarget(name, target); !checked)
            {
                return checked;
            }
        }
        return {};
    }

    json Of(const ProjectFile &project)
    {
        const auto build = project.unknownKeys.find("build");
        if (build == project.unknownKeys.end() || !build->is_object())
        {
            return json::object();
        }
        const auto targets = build->find("targets");
        return targets == build->end() ? json::object() : *targets;
    }

    void Set(ProjectFile &project, const json &targets)
    {
        json &keys = project.unknownKeys;
        if (targets.is_object() && targets.empty())
        {
            if (const auto build = keys.find("build"); build != keys.end() && build->is_object())
            {
                build->erase("targets");
                if (build->empty())
                {
                    keys.erase("build");
                }
            }
            return;
        }
        if (!keys.contains("build") || !keys["build"].is_object())
        {
            keys["build"] = json::object();
        }
        keys["build"]["targets"] = targets;
    }

    std::expected<json, std::string> ApplyPatch(const json &current, const json &patch)
    {
        if (!patch.is_object())
        {
            return std::unexpected("The build targets patch must be a JSON object (RFC 7386 merge patch)");
        }
        json result = current.is_object() ? current : json::object();
        result.merge_patch(patch);
        if (auto checked = Validate(result); !checked)
        {
            return std::unexpected(checked.error());
        }
        return result;
    }
}
