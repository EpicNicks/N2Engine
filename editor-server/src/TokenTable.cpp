#include "editor-server/TokenTable.hpp"

#include <array>
#include <format>

#include "editor-server/EditorTls.hpp"

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

        int ParseEditorRole(const std::string_view text)
        {
            const std::optional<TokenRole> role = ParseTokenRole(text);
            return role ? static_cast<int>(*role) : -1;
        }
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

    const Security::TokenFileFormat &EditorTokenRoles::Format()
    {
        static const Security::TokenFileFormat format{
            &ParseEditorRole,
            "admin, editor or viewer",
            "make the entry with N2EditorHost --new-token",
        };
        return format;
    }

    std::optional<TokenHash> HashToken(const std::string_view token)
    {
        TokenHash digest{};
        if (!EditorTlsServer::Sha256(token, digest))
            return std::nullopt;
        return digest;
    }

    std::expected<TokenTable, std::string> LoadTokensFile(const std::filesystem::path &path)
    {
        return TokenTable::Load(path);
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
