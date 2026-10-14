#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Editor
{
    /**
     * Per-user access tokens (--tokens-file): who may connect to the editor host, as what, and the file that says so.
     * Everything here is plain logic with no sockets, so it is unit tested on its own; EditorServer owns the table and
     * binds a role to each connection at Hello. The SHA-256 of a presented token comes from EditorTlsServer::Sha256
     * (Mbed TLS), so only a build with N2ENGINE_EDITOR_TLS can hash a token; parsing and checking a file needs no hash.
     */

    /// What a user may do. Ordered by privilege: a role may do everything the ones before it may.
    enum class TokenRole : uint8_t
    {
        /// Reads only, and may not play (the play commands are writes: they run the project's Lua on the host)
        Viewer,
        /// Reads and writes: everything except the commands that control the host itself (Shutdown)
        Editor,
        /// Everything
        Admin,
    };

    /// "viewer", "editor" or "admin": how the tokens file, the logs and ServerInfo spell a role
    [[nodiscard]] std::string_view TokenRoleName(TokenRole role);
    /// The role a name spells, exactly (lowercase); nullopt for anything else
    [[nodiscard]] std::optional<TokenRole> ParseTokenRole(std::string_view text);

    inline constexpr std::size_t TokenHashSize = 32;
    /// A SHA-256 digest
    using TokenHash = std::array<unsigned char, TokenHashSize>;
    /// A hash in the tokens file is written "sha256:" and 64 lowercase hex digits
    inline constexpr std::string_view TokenHashPrefix = "sha256:";
    /// A user name is 1 to this many characters of A-Z a-z 0-9 . _ @ - (so it is safe in a log line and a JSON string)
    inline constexpr std::size_t MaxTokenUserLength = 64;
    /// A tokens file larger than this is refused unread (a real one is a few hundred bytes per user)
    inline constexpr std::uintmax_t MaxTokensFileBytes = 1024 * 1024;
    /// The user name --token-env's single token stands for
    inline constexpr std::string_view LegacyTokenUser = "token";

    [[nodiscard]] bool IsValidTokenUserName(std::string_view user);
    /// "sha256:<64 lowercase hex digits>"
    [[nodiscard]] std::string FormatTokenHash(const TokenHash &hash);
    /// The digest of a "sha256:<64 lowercase hex digits>" text; nullopt for anything else (short, long, uppercase, not
    /// hex, another prefix)
    [[nodiscard]] std::optional<TokenHash> ParseTokenHash(std::string_view text);
    /// Compares every byte whatever they hold (the time doesn't depend on where they first differ)
    [[nodiscard]] bool TokenHashesEqual(const TokenHash &a, const TokenHash &b);
    /// SHA-256 of a presented token's bytes, with EditorTlsServer::Sha256; nullopt when this build has no TLS library
    [[nodiscard]] std::optional<TokenHash> HashToken(std::string_view token);

    struct TokenEntry
    {
        std::string user;
        TokenRole role = TokenRole::Viewer;
        TokenHash hash{};
    };

    /// The users of a tokens file. Immutable once made: EditorServer swaps whole tables.
    class TokenTable
    {
    public:
        /**
         * The table a tokens file's text describes: a JSON array of {"user": ..., "role": ..., "hash": ...} objects. An
         * error (no table) for anything else: text that isn't JSON, a top level that isn't an array, an entry that
         * isn't an object with exactly those three string keys, a user name that isn't valid, a role other than admin,
         * editor or viewer, a hash that isn't "sha256:" and 64 lowercase hex digits, a user listed twice, or two users
         * with the same hash. An empty array is a table with no users (nobody can connect; it is how every user is
         * revoked). The error names the entry's number and what is wrong, and never repeats a hash or the text of the
         * file, so it is safe to log.
         */
        [[nodiscard]] static std::expected<TokenTable, std::string> Parse(std::string_view text);

        [[nodiscard]] const std::vector<TokenEntry> &Entries() const { return _entries; }
        [[nodiscard]] std::size_t Size() const { return _entries.size(); }
        /// The entry of this user, or null. A user name isn't secret, so this compares them the ordinary way.
        [[nodiscard]] const TokenEntry *Find(std::string_view user) const;

        /**
         * The entry whose hash is `presented` (the SHA-256 of the token a client sent) and, when `user` is given, whose
         * user is that one: with a user, only that user's entry is considered; without one, the token is matched
         * across every entry. The hash of EVERY entry is compared, in full, in a loop with no early exit, so the time
         * taken depends on the number of entries and not on which one matched or how much of a wrong token was right.
         * (User names aren't secret and are compared the ordinary way.) Nullopt when nothing matches.
         */
        [[nodiscard]] std::optional<TokenEntry> Authenticate(const TokenHash &presented,
                                                             std::optional<std::string_view> user) const;

    private:
        std::vector<TokenEntry> _entries;
    };

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

    /**
     * The table the file holds: an error when it isn't a regular file, is too large (MaxTokensFileBytes), can't be read
     * or doesn't parse (TokenTable::Parse), and, on POSIX, when its group or others can read or write it (a token file
     * must be private to its owner, like an ssh key; Windows ACLs aren't checked). The error names the file.
     *
     * On POSIX the file is opened once and everything is checked on that open file (fstat), then read from it, so the
     * file that was checked is the file that is read: swapping the path for a symbolic link or another file between a
     * check and a read can't get past the permission check. A symbolic link is followed, and its target checked.
     */
    [[nodiscard]] std::expected<TokenTable, std::string> LoadTokensFile(const std::filesystem::path &path);

    /// A token --new-token made, and the line to add to the tokens file for it
    struct NewToken
    {
        /// 64 lowercase hex digits: 256 random bits from the operating system's entropy source (through the TLS library)
        std::string token;
        /// {"user":"<user>","role":"<role>","hash":"sha256:<hex>"}
        std::string entryLine;
    };

    /// A fresh token for this user and role. An error for an invalid user name, or a build without TLS (no SHA-256 and
    /// no random generator), or when the random generator fails. The hash needs no salt: the token is 256 random bits,
    /// so there is nothing to guess.
    [[nodiscard]] std::expected<NewToken, std::string> GenerateToken(std::string_view user, TokenRole role);

    /// What N2EditorHost --new-token prints: the token (once: the host keeps only its hash), then the line to add
    [[nodiscard]] std::string FormatNewTokenOutput(std::string_view user, TokenRole role, const NewToken &token);
}
