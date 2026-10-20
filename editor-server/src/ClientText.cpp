#include "editor-server/ClientText.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "editor-server/EditorServer.hpp"
#include "editor-server/ServerLog.hpp"

namespace N2Engine::Editor::ClientText
{
    namespace
    {
        thread_local bool CurrentLocal = false;
        thread_local std::vector<HostRoot> CurrentRoots;

        /// The path's text with '/' separators and no trailing one
        std::string GenericText(const std::filesystem::path &path)
        {
            const std::u8string text = path.generic_u8string();
            std::string result(text.begin(), text.end());
            while (result.size() > 1 && result.back() == '/')
            {
                result.pop_back();
            }
            return result;
        }

        bool SameChar(const char a, const char b)
        {
#ifdef _WIN32
            // A drive letter and a folder's case are the same folder on Windows
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
#else
            return a == b;
#endif
        }

        size_t Find(const std::string &text, const std::string &needle, const size_t from)
        {
            if (needle.empty() || text.size() < needle.size())
            {
                return std::string::npos;
            }
            for (size_t i = from; i + needle.size() <= text.size(); ++i)
            {
                size_t k = 0;
                while (k < needle.size() && SameChar(text[i + k], needle[k]))
                {
                    ++k;
                }
                if (k == needle.size())
                {
                    return i;
                }
            }
            return std::string::npos;
        }

        /// Whether the text at `at` continues a folder's name, so "/p/proj" is not a prefix of "/p/project2" or "/p/proj.bak"
        /// (a '.' that ends a sentence does not)
        bool ContinuesName(const std::string &text, const size_t at)
        {
            if (at >= text.size())
            {
                return false;
            }
            const char c = text[at];
            const auto u = static_cast<unsigned char>(c);
            if (c == '.')
            {
                return at + 1 < text.size() && std::isalnum(static_cast<unsigned char>(text[at + 1])) != 0;
            }
            return std::isalnum(u) != 0 || c == '_' || c == '-' || u >= 0x80;
        }

        /// Whether the folder found at `at` of the (separator-normalised) text is only the tail of something longer: a
        /// name, a drive ("D:/srv/game" is not "/srv/game"), or a path or URL whose slashes run into it (a client's own
        /// "res://srv/game/a.txt" is not the host's "/srv/game"). A file:// URL is still the host's folder.
        bool StartsInsideName(const std::string &normalized, const size_t at)
        {
            if (at == 0)
            {
                return false;
            }
            const char c = normalized[at - 1];
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.' || c == ':' || u >= 0x80)
            {
                return true;
            }
            if (c != '/')
            {
                return false;
            }
            if (at >= 4 && normalized.compare(at - 4, 4, "//?/") == 0)
            {
                // A Windows long path ("\\?\C:\srv\game") is the host's folder
                return false;
            }
            size_t k = at;
            while (k > 0 && normalized[k - 1] == '/')
            {
                --k;
            }
            constexpr std::string_view scheme = "file:";
            if (k < scheme.size())
            {
                return true;
            }
            for (size_t i = 0; i < scheme.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(normalized[k - scheme.size() + i])) != scheme[i])
                {
                    return true;
                }
            }
            return false;
        }

        /// A folder that is the top of a drive or of the file system ("/", "C:\", "C:"), or too short to be told apart
        /// from ordinary text: it would match nearly anything, so it is never replaced
        bool TooBroadToReplace(const HostRoot &root, std::string spelled)
        {
            while (!spelled.empty() && (spelled.back() == '/' || spelled.back() == '\\'))
            {
                spelled.pop_back();
            }
            return spelled.size() < 3 || root.path.relative_path().empty();
        }

        /// Where the path that follows a replaced folder ends
        bool EndsPath(const char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\'' || c == '"' || c == ')' || c == ']' ||
                   c == '>' || c == ',' || c == ';';
        }
    }

    Scope::Scope(const bool local, std::vector<HostRoot> roots)
        : _previousLocal(CurrentLocal), _previousRoots(std::move(CurrentRoots))
    {
        CurrentLocal = local;
        CurrentRoots = std::move(roots);
    }

    Scope::~Scope()
    {
        CurrentLocal = _previousLocal;
        CurrentRoots = std::move(_previousRoots);
    }

    bool IsLocal()
    {
        return CurrentLocal;
    }

    std::string Failure(const std::string_view what, const std::string_view reason, const std::string_view detail)
    {
        // The detail is the host's to read, whoever asked: it names host paths, so it is an Admin line (PollEvents gives
        // it to an admin, or to a host without authentication, and to no other role)
        const std::string logged = what.empty() ? std::string(detail) : std::string(what) + ": " + std::string(detail);
        ServerLog::Warn(EditorServer::SanitizeForLog(logged, 300));

        const std::string_view shown = IsLocal() && !detail.empty() ? detail : reason;
        if (what.empty())
        {
            return std::string(shown);
        }
        return std::string(what) + ": " + std::string(shown);
    }

    std::string RedactHostPaths(std::string text, const std::vector<HostRoot> &roots)
    {
        // The longest folder first, so <root>/assets is "res://" before <root> is "<project>"
        std::vector<std::pair<std::string, std::string>> forms;
        for (const HostRoot &root : roots)
        {
            std::string spelled = GenericText(root.path);
            if (!TooBroadToReplace(root, spelled))
            {
                forms.emplace_back(std::move(spelled), root.label);
            }
        }
        std::ranges::stable_sort(forms, [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });

        for (const auto &[folder, label] : forms)
        {
            std::string normalized = text;
            std::ranges::replace(normalized, '\\', '/');
            std::string result;
            size_t at = 0;
            for (;;)
            {
                const size_t found = Find(normalized, folder, at);
                if (found == std::string::npos)
                {
                    break;
                }
                const size_t end = found + folder.size();
                result.append(text, at, found - at);
                if (ContinuesName(text, end) || StartsInsideName(normalized, found))
                {
                    // Only the start or the tail of a longer name or path: not this folder
                    result.append(text, found, end - found);
                    at = end;
                    continue;
                }
                result += label;
                size_t k = end;
                if (k < text.size() && (text[k] == '/' || text[k] == '\\'))
                {
                    if (label.empty() || label.back() != '/')
                    {
                        result += '/';
                    }
                    ++k;
                }
                // What follows is a path below the folder: its separators are '/' too
                while (k < text.size() && !EndsPath(text[k]))
                {
                    result += text[k] == '\\' ? '/' : text[k];
                    ++k;
                }
                at = k;
            }
            if (at == 0)
            {
                continue;
            }
            result.append(text, at, std::string::npos);
            text = std::move(result);
        }
        return text;
    }

    std::string Redact(std::string text)
    {
        if (IsLocal())
        {
            return text;
        }
        return RedactHostPaths(std::move(text), CurrentRoots);
    }

    std::string Reason(const std::error_code &error)
    {
        if (error == std::errc::no_such_file_or_directory)
        {
            return "file not found";
        }
        if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
            error == std::errc::read_only_file_system)
        {
            return "permission denied";
        }
        if (error == std::errc::no_space_on_device)
        {
            return "the disk is full";
        }
        return "the file system refused it";
    }

    std::string RelativeTo(const std::filesystem::path &file, const std::filesystem::path &root)
    {
        const std::filesystem::path relative = file.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..")
        {
            return {};
        }
        return GenericText(relative);
    }
}
