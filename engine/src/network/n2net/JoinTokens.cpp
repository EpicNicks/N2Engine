#include "engine/network/n2net/JoinTokens.hpp"

#include <span>

#include "engine/security/Crypto.hpp"

namespace N2Engine::N2Net
{
    namespace
    {
        int ParseJoinRoleCode(std::string_view text)
        {
            const std::optional<JoinRole> role = ParseJoinRole(text);
            return role ? static_cast<int>(*role) : -1;
        }
    } // namespace

    std::string_view JoinRoleName(JoinRole role)
    {
        switch (role)
        {
        case JoinRole::Player:
            return "player";
        case JoinRole::Spectator:
            return "spectator";
        case JoinRole::Admin:
            return "admin";
        }
        return "player";
    }

    std::optional<JoinRole> ParseJoinRole(std::string_view text)
    {
        if (text == "player")
        {
            return JoinRole::Player;
        }
        if (text == "spectator")
        {
            return JoinRole::Spectator;
        }
        if (text == "admin")
        {
            return JoinRole::Admin;
        }
        return std::nullopt;
    }

    const Security::TokenFileFormat &JoinTokenRoles::Format()
    {
        static const Security::TokenFileFormat format{
            &ParseJoinRoleCode,
            "admin, player or spectator",
            "the SHA-256 of the token's text",
        };
        return format;
    }

    std::optional<Security::TokenHash> HashJoinToken(std::string_view token)
    {
        Security::Sha256Digest digest{};
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(token.data()), token.size());
        if (!Security::Sha256(bytes, digest))
        {
            return std::nullopt;
        }
        Security::TokenHash hash{};
        for (std::size_t i = 0; i < hash.size(); ++i)
        {
            hash[i] = digest[i];
        }
        Security::SecureZero(digest.data(), digest.size());
        return hash;
    }
} // namespace N2Engine::N2Net
