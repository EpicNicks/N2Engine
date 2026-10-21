#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "engine/security/TokenTable.hpp"

namespace N2Engine::N2Net
{
    /**
     * Join tokens: who may join a game server, and as what. A static tokens file (v1), the same format and the same
     * checks as the editor host's --tokens-file (n2security's TokenTable: a JSON array of {"user", "role", "hash"}
     * objects, the hash "sha256:" and the hex SHA-256 of the token's text, the file private to its owner on POSIX), with
     * the game's roles. The server keeps only the hashes; a client presents its token through SecureTransport's
     * handshake, which never sends the token or its hash (it proves knowledge of the hash with HMACs, see
     * docs/netcode.html). Nothing here logs, and no error repeats a hash or the file's text.
     */
    enum class JoinRole : std::uint8_t
    {
        /// Plays
        Player,
        /// Watches
        Spectator,
        /// Plays and may run the session (kick, change the map: what admin may do is the game's and later work's)
        Admin,
    };

    /// "player", "spectator" or "admin": how the tokens file spells a role
    [[nodiscard]] std::string_view JoinRoleName(JoinRole role);
    /// The role a name spells, exactly (lowercase); nullopt for anything else
    [[nodiscard]] std::optional<JoinRole> ParseJoinRole(std::string_view text);

    /// The game's spelling of its roles in a tokens file
    struct JoinTokenRoles
    {
        using Role = JoinRole;
        [[nodiscard]] static const Security::TokenFileFormat &Format();
    };

    using JoinTokenEntry = Security::BasicTokenEntry<JoinRole>;
    /// JoinTokenTable::Parse(text) and JoinTokenTable::Load(path) (with the file checks); immutable once made
    using JoinTokenTable = Security::BasicTokenTable<JoinTokenRoles>;

    /// The SHA-256 of a token's text, as a tokens file holds it; nullopt in a build without the crypto library (web)
    [[nodiscard]] std::optional<Security::TokenHash> HashJoinToken(std::string_view token);
} // namespace N2Engine::N2Net
