#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "engine/security/TokenTable.hpp"

namespace N2Engine::Editor
{
    /**
     * Per-user access tokens (--tokens-file): who may connect to the editor host, as what, and the file that says so.
     * Everything here is plain logic with no sockets, so it is unit tested on its own; EditorServer owns the table and
     * binds a role to each connection at Hello. The parsing, the file checks and the constant-time matching are
     * n2security's (engine/security/TokenTable.hpp, shared with the netcode's join tokens); this header gives them the
     * editor's roles and names. The SHA-256 of a presented token comes from EditorTlsServer::Sha256 (Mbed TLS), so only
     * a build with N2ENGINE_EDITOR_TLS can hash a token; parsing and checking a file needs no hash.
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

    using Security::TokenHashSize;
    /// A SHA-256 digest
    using Security::TokenHash;
    using Security::TokenHashPrefix;
    using Security::MaxTokenUserLength;
    using Security::MaxTokensFileBytes;
    /// The user name --token-env's single token stands for
    inline constexpr std::string_view LegacyTokenUser = "token";

    using Security::IsValidTokenUserName;
    using Security::FormatTokenHash;
    using Security::ParseTokenHash;
    using Security::TokenHashesEqual;
    /// SHA-256 of a presented token's bytes, with EditorTlsServer::Sha256; nullopt when this build has no TLS library
    [[nodiscard]] std::optional<TokenHash> HashToken(std::string_view token);

    /// The editor's spelling of its roles in a tokens file
    struct EditorTokenRoles
    {
        using Role = TokenRole;
        [[nodiscard]] static const Security::TokenFileFormat &Format();
    };

    using TokenEntry = Security::BasicTokenEntry<TokenRole>;
    /// The users of a tokens file. Immutable once made: EditorServer swaps whole tables. TokenTable::Parse refuses a role
    /// other than admin, editor or viewer (see Security::ParseTokenRecords for the rest).
    using TokenTable = Security::BasicTokenTable<EditorTokenRoles>;

    using Security::TokensFileSignature;
    using Security::StatTokensFile;

    /// The table the file holds (Security::ReadTokensFileText's checks, then TokenTable::Parse); the error names the file
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
