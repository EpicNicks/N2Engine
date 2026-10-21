#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// n2security: access-token files shared by the editor host (--tokens-file, roles viewer/editor/admin) and the netcode's
// join tokens (roles player/spectator/admin). Moved here from editor-server/ (TokenTable.hpp/.cpp): the parsing, the file
// checks and the constant-time matching are the editor's code unchanged, made generic over the role names. Plain logic
// with no sockets, so it is unit tested on its own. Hashing a presented token is the user's business: the editor host
// keeps its own HashToken (Mbed TLS through EditorTlsServer, only with N2ENGINE_EDITOR_TLS), the netcode uses
// Security::Sha256 (Crypto.hpp). Nothing here logs; no error message ever repeats a hash or the text of a file.
namespace N2Engine::Security
{
    inline constexpr std::size_t TokenHashSize = 32;
    /// A SHA-256 digest
    using TokenHash = std::array<unsigned char, TokenHashSize>;
    /// A hash in a tokens file is written "sha256:" and 64 lowercase hex digits
    inline constexpr std::string_view TokenHashPrefix = "sha256:";
    /// A user name is 1 to this many characters of A-Z a-z 0-9 . _ @ - (so it is safe in a log line and a JSON string)
    inline constexpr std::size_t MaxTokenUserLength = 64;
    /// A tokens file larger than this is refused unread (a real one is a few hundred bytes per user)
    inline constexpr std::uintmax_t MaxTokensFileBytes = 1024 * 1024;

    [[nodiscard]] bool IsValidTokenUserName(std::string_view user);
    /// "sha256:<64 lowercase hex digits>"
    [[nodiscard]] std::string FormatTokenHash(const TokenHash &hash);
    /// The digest of a "sha256:<64 lowercase hex digits>" text; nullopt for anything else (short, long, uppercase, not
    /// hex, another prefix)
    [[nodiscard]] std::optional<TokenHash> ParseTokenHash(std::string_view text);
    /// Compares every byte whatever they hold (the time doesn't depend on where they first differ)
    [[nodiscard]] bool TokenHashesEqual(const TokenHash &a, const TokenHash &b);

    /// How one kind of tokens file spells its roles, and what its errors suggest
    struct TokenFileFormat
    {
        /// The role a name spells, exactly (lowercase), as the role enum's value; -1 for anything else
        int (*parseRole)(std::string_view name) = nullptr;
        /// Ends "the role must be ": "admin, editor or viewer"
        std::string_view roleList;
        /// In brackets after "the hash must be "sha256:" and 64 lowercase hex digits": how to make an entry
        std::string_view hashHint;
    };

    /// One entry of a tokens file, its role as TokenFileFormat::parseRole returned it
    struct TokenRecord
    {
        std::string user;
        int role = 0;
        TokenHash hash{};
    };

    /**
     * The entries a tokens file's text describes: a JSON array of {"user": ..., "role": ..., "hash": ...} objects. An
     * error for anything else: text that isn't JSON, a top level that isn't an array, an entry that isn't an object with
     * exactly those three string keys, a user name that isn't valid, a role `format` doesn't know, a hash that isn't
     * "sha256:" and 64 lowercase hex digits, a user listed twice, or two users with the same hash. An empty array is a
     * table with no users (nobody can connect; it is how every user is revoked). The error names the entry's number and
     * what is wrong, and never repeats a hash or the text of the file, so it is safe to log.
     */
    [[nodiscard]] std::expected<std::vector<TokenRecord>, std::string> ParseTokenRecords(std::string_view text,
                                                                                         const TokenFileFormat &format);

    /**
     * The text of a tokens file: an error when it isn't a regular file, is too large (MaxTokensFileBytes) or can't be
     * read, and, on POSIX, when its group or others can read or write it (a token file must be private to its owner,
     * like an ssh key; Windows ACLs aren't checked). On POSIX the file is opened once and everything is checked on that
     * open file (fstat), then read from it, so the file that was checked is the file that is read. A symbolic link is
     * followed, and its target checked. The error does not name the file (BasicTokenTable::Load adds that).
     */
    [[nodiscard]] std::expected<std::string, std::string> ReadTokensFileText(const std::filesystem::path &path);

    /// What identifies one version of a tokens file: whether it exists, its size and its modification time
    struct TokensFileSignature
    {
        bool exists = false;
        std::uint64_t size = 0;
        std::int64_t modified = 0;

        [[nodiscard]] bool operator==(const TokensFileSignature &other) const = default;
    };

    /// The file's signature now (not existing is a signature too)
    [[nodiscard]] TokensFileSignature StatTokensFile(const std::filesystem::path &path);

    template <typename Role>
    struct BasicTokenEntry
    {
        std::string user;
        Role role{};
        TokenHash hash{};
    };

    /**
     * The users of a tokens file. Immutable once made: a holder swaps whole tables. `Traits` names the role type
     * (`using Role = ...`, an enum whose values are what Traits::Format().parseRole returns) and the file's spelling of
     * it (`static const TokenFileFormat &Format()`).
     */
    template <typename Traits>
    class BasicTokenTable
    {
    public:
        using Role = typename Traits::Role;
        using Entry = BasicTokenEntry<Role>;

        /// See ParseTokenRecords
        [[nodiscard]] static std::expected<BasicTokenTable, std::string> Parse(const std::string_view text)
        {
            auto records = ParseTokenRecords(text, Traits::Format());
            if (!records)
                return std::unexpected(std::move(records.error()));
            BasicTokenTable table;
            table._entries.reserve(records->size());
            for (TokenRecord &record : *records)
                table._entries.push_back(Entry{std::move(record.user), static_cast<Role>(record.role), record.hash});
            return table;
        }

        /// The table the file holds (ReadTokensFileText, then Parse); the error names the file
        [[nodiscard]] static std::expected<BasicTokenTable, std::string> Load(const std::filesystem::path &path)
        {
            const std::string name = path.string();
            const auto text = ReadTokensFileText(path);
            if (!text)
                return std::unexpected(std::format("tokens file {}: {}", name, text.error()));
            auto table = Parse(*text);
            if (!table)
                return std::unexpected(std::format("tokens file {}: {}", name, table.error()));
            return table;
        }

        [[nodiscard]] const std::vector<Entry> &Entries() const { return _entries; }
        [[nodiscard]] std::size_t Size() const { return _entries.size(); }

        /// The entry of this user, or null. A user name isn't secret, so this compares them the ordinary way.
        [[nodiscard]] const Entry *Find(const std::string_view user) const
        {
            for (const Entry &entry : _entries)
            {
                if (entry.user == user)
                    return &entry;
            }
            return nullptr;
        }

        /**
         * The entry whose hash is `presented` (the SHA-256 of the token a client sent) and, when `user` is given, whose
         * user is that one: with a user, only that user's entry is considered; without one, the token is matched
         * across every entry. The hash of EVERY entry is compared, in full, in a loop with no early exit, so the time
         * taken depends on the number of entries and not on which one matched or how much of a wrong token was right.
         * (User names aren't secret and are compared the ordinary way.) Nullopt when nothing matches.
         */
        [[nodiscard]] std::optional<Entry> Authenticate(const TokenHash &presented,
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

    private:
        std::vector<Entry> _entries;
    };
}
