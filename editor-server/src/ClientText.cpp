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

        bool IsNameChar(const char c)
        {
            const auto u = static_cast<unsigned char>(c);
            return std::isalnum(u) != 0 || c == '_' || c == '-' || u >= 0x80;
        }

        /// Whether the folder found at `at` of the normalised text is only the tail of something longer, so it is not
        /// the host's folder: a name ("/p/xgame"), a path that goes on from further left ("/data/srv/game", a client's
        /// own "res://a/srv/game/b.txt"), a drive ("D:/srv/game" is not "/srv/game"), a relative path ("../srv/game") or
        /// the client's own res:// or user:// path ("res://srv/game/a.txt"). A folder after anything else is the host's:
        /// a space, a quote, a bracket, '=', ':' ("open:/srv/game"), a full stop ("failed./srv/game") or a slash run
        /// that is file:// or a Windows long-path prefix, so those are replaced.
        bool StartsInsideName(const std::string &t, const size_t at, const bool folderStartsWithSlash)
        {
            if (at == 0)
            {
                return false;
            }
            const char c = t[at - 1];
            if (IsNameChar(c))
            {
                return true;
            }
            if (c == '.')
            {
                if (!folderStartsWithSlash)
                {
                    return false;
                }
                // "./srv/game" and "../srv/game" are relative paths, but "failed./srv/game" has a name before the dot
                size_t k = at - 1;
                while (k > 0 && t[k - 1] == '.')
                {
                    --k;
                }
                return at - k <= 2 && (k == 0 || !IsNameChar(t[k - 1]));
            }
            if (c == ':')
            {
                // Only a drive letter makes "X:/srv/game" another path than "/srv/game"
                return folderStartsWithSlash && at >= 2 && std::isalpha(static_cast<unsigned char>(t[at - 2])) != 0 &&
                       (at == 2 || !IsNameChar(t[at - 3]));
            }
            if (c != '/')
            {
                return false;
            }
            if (at >= 3 && t.compare(at - 3, 3, "/?/") == 0)
            {
                // A Windows long path ("\\?\C:\srv\game") is the host's folder
                return false;
            }
            size_t k = at;
            while (k > 0 && t[k - 1] == '/')
            {
                --k;
            }
            if (k == 0)
            {
                return false;
            }
            const char before = t[k - 1];
            if (before == ':')
            {
                // The slashes follow a scheme: only the client's own res:// and user:// protect the folder
                size_t s = k - 1;
                while (s > 0 && std::isalpha(static_cast<unsigned char>(t[s - 1])) != 0)
                {
                    --s;
                }
                std::string scheme = t.substr(s, k - 1 - s);
                std::ranges::transform(scheme, scheme.begin(),
                                       [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return scheme == "res" || scheme == "user" || (scheme.size() == 1 && folderStartsWithSlash);
            }
            return IsNameChar(before) || before == '.';
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

        /// A message's text as the folders are matched in it: percent-encoded characters decoded ("%5C", "%2F", "%3A"),
        /// each '\' as '/', and with `collapse` a run of backslashes written for each separator in a C or JSON string
        /// ("C:\\proj" is "C:\proj") counted as half as many. `origin[i]` is where character i starts in the original
        /// text and `origin.back()` is its length, so a stretch of the normalised text is a stretch of the original.
        struct Normalised
        {
            std::string text;
            std::vector<size_t> origin;
        };

        int HexValue(const char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }

        Normalised Normalise(const std::string &text, const bool collapse)
        {
            std::string decoded;
            std::vector<size_t> where;
            for (size_t i = 0; i < text.size();)
            {
                if (text[i] == '%' && i + 2 < text.size())
                {
                    const int high = HexValue(text[i + 1]);
                    const int low = HexValue(text[i + 2]);
                    if (high >= 0 && low >= 0 && (high != 0 || low != 0))
                    {
                        decoded.push_back(static_cast<char>(high * 16 + low));
                        where.push_back(i);
                        i += 3;
                        continue;
                    }
                }
                decoded.push_back(text[i]);
                where.push_back(i);
                ++i;
            }

            Normalised result;
            for (size_t i = 0; i < decoded.size();)
            {
                if (decoded[i] != '\\')
                {
                    result.text.push_back(decoded[i]);
                    result.origin.push_back(where[i]);
                    ++i;
                    continue;
                }
                size_t run = 0;
                while (i + run < decoded.size() && decoded[i + run] == '\\')
                {
                    ++run;
                }
                const size_t step = collapse ? 2 : 1;
                for (size_t j = 0; j * step < run; ++j)
                {
                    result.text.push_back('/');
                    result.origin.push_back(where[i + j * step]);
                }
                i += run;
            }
            result.origin.push_back(text.size());
            return result;
        }

        /// Where the path that follows a replaced folder ends
        bool EndsPath(const char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\'' || c == '"' || c == ')' || c == ']' ||
                   c == '>' || c == ',' || c == ';';
        }

        /// `text` with each occurrence of `folder` (and the path below it) written as `label`
        std::string ReplaceFolder(const std::string &text, const std::string &folder, const std::string &label,
                                  const bool collapse)
        {
            const Normalised n = Normalise(text, collapse);
            std::string result;
            size_t at = 0;
            size_t from = 0;
            bool changed = false;
            for (;;)
            {
                const size_t found = Find(n.text, folder, from);
                if (found == std::string::npos)
                {
                    break;
                }
                const size_t end = found + folder.size();
                if (ContinuesName(n.text, end) || StartsInsideName(n.text, found, folder.front() == '/'))
                {
                    // Only the start or the tail of a longer name or path: not this folder
                    from = end;
                    continue;
                }
                changed = true;
                result.append(text, at, n.origin[found] - at);
                result += label;
                size_t k = end;
                if (k < n.text.size() && n.text[k] == '/')
                {
                    if (label.empty() || label.back() != '/')
                    {
                        result += '/';
                    }
                    ++k;
                }
                // What follows is a path below the folder: its separators are '/' too
                while (k < n.text.size() && !EndsPath(n.text[k]))
                {
                    result += n.text[k];
                    ++k;
                }
                at = n.origin[k];
                from = k;
            }
            if (!changed)
            {
                return text;
            }
            result.append(text, at, std::string::npos);
            return result;
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
            // Once as the text is written, then with its doubled backslashes (an escaped string) counted as one
            text = ReplaceFolder(text, folder, label, false);
            text = ReplaceFolder(text, folder, label, true);
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
