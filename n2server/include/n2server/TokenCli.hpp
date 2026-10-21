#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "engine/network/n2net/JoinTokens.hpp"
#include "n2server/ServerOptions.hpp"

namespace N2Engine::Server
{
    /**
     * The join-token generator (N2Server --new-token <user> <role> --tokens-file <path>): the counterpart of the editor
     * host's --new-token, for the game's roles (player, spectator, admin). It makes a token of 256 random bits, adds the
     * entry {"user", "role", "hash"} (the hash is "sha256:" and the SHA-256 of the token's text, what
     * N2Net::JoinTokenTable reads) to the tokens file, and hands the token back to be shown once. The file keeps only
     * the hash; the token is never written anywhere, logged, or part of an error message.
     */

    /// A token made, and the line the tokens file now holds for it
    struct NewJoinToken
    {
        /// 64 lowercase hex digits: 256 random bits from the operating system's entropy source (through n2security)
        std::string token;
        /// {"user":"<user>","role":"<role>","hash":"sha256:<hex>"}
        std::string entryLine;
    };

    /// A fresh token for this user and role, not stored anywhere. An error for an invalid user name, or when the random
    /// generator or SHA-256 fails (or this build has no crypto library). The hash needs no salt: the token is 256 random
    /// bits, so there is nothing to guess.
    [[nodiscard]] std::expected<NewJoinToken, std::string> GenerateJoinToken(std::string_view user, N2Net::JoinRole role);

    /// The text of a tokens file holding `table`'s users followed by `added`, one entry per line:
    /// "[\n  {...},\n  {...}\n]\n". Entries are written from the parsed table, so the file is always well formed.
    [[nodiscard]] std::string FormatTokensFile(const N2Net::JoinTokenTable &table,
                                               const N2Net::JoinTokenEntry &added);

    /**
     * Makes a token for `user` and adds it to the tokens file at `path` (created when it doesn't exist, private to its
     * owner: mode 0600 on POSIX, with the ACL of its folder on Windows). An existing file is read with the same checks
     * the server makes (a regular file, small enough, private to its owner on POSIX, well formed), so a file the server
     * would refuse is refused here too, unchanged; and a user that is already in it is an error (revoke by editing the
     * file). The new file is written beside the old one and moved over it, so a reader never sees half a file; two
     * --new-token runs at the same time can lose one entry, so run them one at a time. The token is returned only here.
     * The error names the file, never a token or a hash.
     */
    [[nodiscard]] std::expected<NewJoinToken, std::string> AddJoinToken(const std::filesystem::path &path,
                                                                        std::string_view user, N2Net::JoinRole role);

    /// What N2Server --new-token prints: the token (once: the file keeps only its hash) on a line of its own,
    /// "token: <hex>", then what was done. A launcher takes the token from that line.
    [[nodiscard]] std::string FormatNewTokenOutput(std::string_view user, N2Net::JoinRole role, const NewJoinToken &token,
                                                   const std::filesystem::path &path);

    /// N2Server --new-token (RunServer calls it when options.newTokenUser is set): AddJoinToken, then FormatNewTokenOutput
    /// to stdout and 0; on a failure one line "N2Server --new-token: <reason>" to stderr and 1. The engine is not started,
    /// and nothing is logged.
    int RunNewToken(const ServerOptions &options);
}
