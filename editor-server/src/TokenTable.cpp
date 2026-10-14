#include "editor-server/TokenTable.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "editor-server/EditorTls.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace N2Engine::Editor
{
    namespace
    {
        constexpr std::string_view HexDigits = "0123456789abcdef";

        std::string ToHex(const unsigned char *data, const std::size_t size)
        {
            std::string text;
            text.reserve(size * 2);
            for (std::size_t i = 0; i < size; ++i)
            {
                text += HexDigits[data[i] >> 4];
                text += HexDigits[data[i] & 0x0F];
            }
            return text;
        }

        /// 0 to 15 for a lowercase hex digit, -1 for anything else
        int HexValue(const char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        }

        constexpr std::string_view Utf8Bom = "\xEF\xBB\xBF";
    }

    std::string_view TokenRoleName(const TokenRole role)
    {
        switch (role)
        {
        case TokenRole::Viewer:
            return "viewer";
        case TokenRole::Editor:
            return "editor";
        case TokenRole::Admin:
            return "admin";
        }
        return "viewer";
    }

    std::optional<TokenRole> ParseTokenRole(const std::string_view text)
    {
        if (text == "viewer")
            return TokenRole::Viewer;
        if (text == "editor")
            return TokenRole::Editor;
        if (text == "admin")
            return TokenRole::Admin;
        return std::nullopt;
    }

    bool IsValidTokenUserName(const std::string_view user)
    {
        if (user.empty() || user.size() > MaxTokenUserLength)
            return false;
        return std::ranges::all_of(user,
                                   [](const char c)
                                   {
                                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '@' || c == '-';
                                   });
    }

    std::string FormatTokenHash(const TokenHash &hash)
    {
        return std::string(TokenHashPrefix) + ToHex(hash.data(), hash.size());
    }

    std::optional<TokenHash> ParseTokenHash(const std::string_view text)
    {
        if (!text.starts_with(TokenHashPrefix) || text.size() != TokenHashPrefix.size() + TokenHashSize * 2)
            return std::nullopt;
        const std::string_view digits = text.substr(TokenHashPrefix.size());
        TokenHash hash{};
        for (std::size_t i = 0; i < TokenHashSize; ++i)
        {
            const int high = HexValue(digits[i * 2]);
            const int low = HexValue(digits[i * 2 + 1]);
            if (high < 0 || low < 0)
                return std::nullopt;
            hash[i] = static_cast<unsigned char>(high << 4 | low);
        }
        return hash;
    }

    bool TokenHashesEqual(const TokenHash &a, const TokenHash &b)
    {
        // Every byte, whatever they hold: no early exit
        unsigned int difference = 0;
        for (std::size_t i = 0; i < TokenHashSize; ++i)
            difference |= static_cast<unsigned int>(a[i] ^ b[i]);
        return difference == 0;
    }

    std::optional<TokenHash> HashToken(const std::string_view token)
    {
        TokenHash digest{};
        if (!EditorTlsServer::Sha256(token, digest))
            return std::nullopt;
        return digest;
    }

    // ==================== TokenTable ====================

    std::expected<TokenTable, std::string> TokenTable::Parse(const std::string_view text)
    {
        // A Windows editor may save the file with a byte order mark; JSON has no room for it
        const std::string_view body = text.starts_with(Utf8Bom) ? text.substr(Utf8Bom.size()) : text;

        // No exceptions, and no parser message: nlohmann's quote the offending text, and a tokens file is a secret's
        // neighbour. The caller learns that the file isn't JSON, nothing of what it holds.
        const nlohmann::json root = nlohmann::json::parse(std::string(body), nullptr, false);
        if (root.is_discarded())
            return std::unexpected("it is not valid JSON");
        if (!root.is_array())
            return std::unexpected("it must be a JSON array of {\"user\", \"role\", \"hash\"} objects");

        TokenTable table;
        std::size_t number = 0;
        for (const nlohmann::json &item : root)
        {
            ++number;
            if (!item.is_object())
                return std::unexpected(std::format("entry {}: it must be an object with user, role and hash", number));
            for (const auto &[key, value] : item.items())
            {
                (void)value;
                if (key != "user" && key != "role" && key != "hash")
                    return std::unexpected(std::format("entry {}: only user, role and hash are allowed", number));
            }
            if (!item.contains("user") || !item.contains("role") || !item.contains("hash"))
                return std::unexpected(std::format("entry {}: user, role and hash are all required", number));
            if (!item.at("user").is_string() || !item.at("role").is_string() || !item.at("hash").is_string())
                return std::unexpected(std::format("entry {}: user, role and hash must be strings", number));

            TokenEntry entry;
            entry.user = item.at("user").get<std::string>();
            if (!IsValidTokenUserName(entry.user))
                return std::unexpected(std::format("entry {}: the user must be 1 to {} characters of A-Z a-z 0-9 . _ @ -",
                                                   number, MaxTokenUserLength));
            const std::optional<TokenRole> role = ParseTokenRole(item.at("role").get<std::string>());
            if (!role)
                return std::unexpected(std::format("entry {} ({}): the role must be admin, editor or viewer", number,
                                                   entry.user));
            entry.role = *role;
            const std::optional<TokenHash> hash = ParseTokenHash(item.at("hash").get<std::string>());
            if (!hash)
                return std::unexpected(std::format("entry {} ({}): the hash must be \"sha256:\" and 64 lowercase hex "
                                                   "digits (make the entry with N2EditorHost --new-token)",
                                                   number, entry.user));
            entry.hash = *hash;

            for (const TokenEntry &earlier : table._entries)
            {
                if (earlier.user == entry.user)
                    return std::unexpected(std::format("entry {}: the user {} is listed twice", number, entry.user));
                if (TokenHashesEqual(earlier.hash, entry.hash))
                    return std::unexpected(std::format("entry {}: {} has the same hash as {} (one token, two users)",
                                                       number, entry.user, earlier.user));
            }
            table._entries.push_back(std::move(entry));
        }
        return table;
    }

    const TokenEntry *TokenTable::Find(const std::string_view user) const
    {
        for (const TokenEntry &entry : _entries)
        {
            if (entry.user == user)
                return &entry;
        }
        return nullptr;
    }

    std::optional<TokenEntry> TokenTable::Authenticate(const TokenHash &presented,
                                                       const std::optional<std::string_view> user) const
    {
        constexpr std::size_t None = static_cast<std::size_t>(-1);
        std::size_t found = None;
        for (std::size_t i = 0; i < _entries.size(); ++i)
        {
            // Both are always evaluated, and the hash comparison always reads all 32 bytes: nothing stops early
            const bool userMatches = !user.has_value() || _entries[i].user == *user;
            const bool hashMatches = TokenHashesEqual(presented, _entries[i].hash);
            const std::size_t mask = static_cast<std::size_t>(0) - static_cast<std::size_t>(userMatches & hashMatches);
            found = (mask & i) | (~mask & found);
        }
        if (found == None)
            return std::nullopt;
        return _entries[found];
    }

    // ==================== The file ====================

    TokensFileSignature StatTokensFile(const fs::path &path)
    {
        TokensFileSignature signature;
        std::error_code error;
        const std::uintmax_t size = fs::file_size(path, error);
        if (error)
            return signature;
        const auto modified = fs::last_write_time(path, error);
        if (error)
            return signature;
        signature.exists = true;
        signature.size = size;
        signature.modified = static_cast<std::int64_t>(modified.time_since_epoch().count());
        return signature;
    }

    namespace
    {
        /// The file's text, or why it can't be used. Everything about the file is decided on one open file, so what is
        /// checked is what is read.
        std::expected<std::string, std::string> ReadTokensFileText(const fs::path &path)
        {
#ifdef _WIN32
            std::error_code error;
            if (!fs::is_regular_file(path, error))
                return std::unexpected("it doesn't exist or isn't a regular file");
            const std::uintmax_t size = fs::file_size(path, error);
            if (error || size > MaxTokensFileBytes)
                return std::unexpected(std::format("it is larger than {} bytes", MaxTokensFileBytes));
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::unexpected("it can't be opened");
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (file.bad() || text.size() > MaxTokensFileBytes)
                return std::unexpected("it can't be read");
            return text;
#else
            const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (descriptor < 0)
                return std::unexpected("it doesn't exist or can't be opened");

            struct Closer
            {
                int descriptor;
                ~Closer() { ::close(descriptor); }
            } const closer{descriptor};

            struct stat info{};
            if (::fstat(descriptor, &info) != 0)
                return std::unexpected("it can't be inspected");
            if (!S_ISREG(info.st_mode))
                return std::unexpected("it doesn't exist or isn't a regular file");
            if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
                return std::unexpected("it is accessible to its group or to others; make it private to its owner "
                                       "(chmod 600), as a token file must be");
            if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > MaxTokensFileBytes)
                return std::unexpected(std::format("it is larger than {} bytes", MaxTokensFileBytes));

            std::string text;
            char buffer[4096];
            for (;;)
            {
                const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return std::unexpected("it can't be read");
                }
                if (count == 0)
                    break;
                text.append(buffer, static_cast<std::size_t>(count));
                if (text.size() > MaxTokensFileBytes)
                    return std::unexpected(std::format("it is larger than {} bytes", MaxTokensFileBytes));
            }
            return text;
#endif
        }
    }

    std::expected<TokenTable, std::string> LoadTokensFile(const fs::path &path)
    {
        const std::string name = path.string();
        const auto text = ReadTokensFileText(path);
        if (!text)
            return std::unexpected(std::format("tokens file {}: {}", name, text.error()));

        auto table = TokenTable::Parse(*text);
        if (!table)
            return std::unexpected(std::format("tokens file {}: {}", name, table.error()));
        return table;
    }

    // ==================== --new-token ====================

    std::expected<NewToken, std::string> GenerateToken(const std::string_view user, const TokenRole role)
    {
        if (!IsValidTokenUserName(user))
            return std::unexpected(std::format("the user must be 1 to {} characters of A-Z a-z 0-9 . _ @ -",
                                               MaxTokenUserLength));
        if (!EditorTlsServer::IsAvailable())
            return std::unexpected("this build has no TLS library, which supplies the random generator and SHA-256 "
                                   "(configure it with -DN2ENGINE_EDITOR_TLS=ON)");

        std::array<unsigned char, TokenHashSize> random{};
        if (!EditorTlsServer::RandomBytes(random.data(), random.size()))
            return std::unexpected("the random generator failed");

        NewToken made;
        made.token = ToHex(random.data(), random.size());
        const std::optional<TokenHash> hash = HashToken(made.token);
        if (!hash)
            return std::unexpected("SHA-256 failed");
        made.entryLine = std::format("{{\"user\":\"{}\",\"role\":\"{}\",\"hash\":\"{}\"}}", user, TokenRoleName(role),
                                     FormatTokenHash(*hash));
        return made;
    }

    std::string FormatNewTokenOutput(const std::string_view user, const TokenRole role, const NewToken &token)
    {
        return std::format("New token for {} ({}). It is shown once: the host keeps only its hash, so store the token "
                           "now.\n"
                           "token: {}\n"
                           "Add this entry to the tokens file (a JSON array), then ask the host to reload it "
                           "(it re-reads the file on each new connection, and on SIGHUP):\n"
                           "{}\n",
                           user, TokenRoleName(role), token.token, token.entryLine);
    }
}
