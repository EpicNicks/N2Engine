#include "n2server/TokenCli.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <system_error>
#include <utility>

#include "engine/security/Crypto.hpp"
#include "engine/security/TokenTable.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace N2Engine::Server
{
    namespace
    {
        constexpr std::size_t kTokenBytes = 32;

        std::string ToHex(const std::uint8_t *bytes, const std::size_t size)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string text;
            text.reserve(size * 2);
            for (std::size_t i = 0; i < size; ++i)
            {
                text.push_back(digits[bytes[i] >> 4]);
                text.push_back(digits[bytes[i] & 0x0F]);
            }
            return text;
        }

        std::string EntryLine(const std::string_view user, const N2Net::JoinRole role, const Security::TokenHash &hash)
        {
            // The user name is restricted to characters that need no JSON escaping (IsValidTokenUserName)
            return std::format("{{\"user\":\"{}\",\"role\":\"{}\",\"hash\":\"{}\"}}", user, N2Net::JoinRoleName(role),
                               Security::FormatTokenHash(hash));
        }

        struct MadeToken
        {
            std::string token;
            Security::TokenHash hash{};
        };

        std::expected<MadeToken, std::string> MakeToken(const std::string_view user)
        {
            if (!Security::IsValidTokenUserName(user))
            {
                return std::unexpected(std::format("the user must be 1 to {} characters of A-Z a-z 0-9 . _ @ -",
                                                   Security::MaxTokenUserLength));
            }
            if (!Security::CryptoAvailable() || !Security::InitializeCrypto())
            {
                return std::unexpected("this build has no crypto library, which supplies the random generator and SHA-256");
            }
            std::array<std::uint8_t, kTokenBytes> random{};
            if (!Security::RandomBytes(random))
            {
                return std::unexpected("the random generator failed");
            }
            MadeToken made;
            made.token = ToHex(random.data(), random.size());
            Security::SecureZero(random.data(), random.size());
            const std::optional<Security::TokenHash> hash = N2Net::HashJoinToken(made.token);
            if (!hash)
            {
                Security::SecureZero(made.token.data(), made.token.size());
                return std::unexpected("SHA-256 failed");
            }
            made.hash = *hash;
            return made;
        }

        [[maybe_unused]] std::string ErrnoText(const int code)
        {
            return std::error_code(code, std::generic_category()).message();
        }

        /// Writes `text` to a new file beside `target` and moves it over `target`. On POSIX the new file is created with
        /// mode 0600 (and O_EXCL, so it is never a file or a link that was there before), written, flushed to disk and
        /// only then renamed; a failure removes it and leaves `target` as it was.
        std::expected<void, std::string> WriteFileAtomically(const std::filesystem::path &target, const std::string &text)
        {
            std::array<std::uint8_t, 8> suffixBytes{};
            if (!Security::RandomBytes(suffixBytes))
            {
                return std::unexpected("the random generator failed");
            }
            std::filesystem::path temporary = target;
            temporary += ".tmp-" + ToHex(suffixBytes.data(), suffixBytes.size());

#if !defined(_WIN32)
            const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0)
            {
                return std::unexpected("can't create a temporary file next to it: " + ErrnoText(errno));
            }
            std::size_t written = 0;
            while (written < text.size())
            {
                const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
                if (count < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    const int code = errno;
                    ::close(fd);
                    std::error_code ignored;
                    std::filesystem::remove(temporary, ignored);
                    return std::unexpected("can't write it: " + ErrnoText(code));
                }
                written += static_cast<std::size_t>(count);
            }
            if (::fsync(fd) != 0)
            {
                const int code = errno;
                ::close(fd);
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return std::unexpected("can't flush it to disk: " + ErrnoText(code));
            }
            if (::close(fd) != 0)
            {
                const int code = errno;
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return std::unexpected("can't close it: " + ErrnoText(code));
            }
#else
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream)
                {
                    return std::unexpected("can't create a temporary file next to it");
                }
                stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                stream.flush();
                if (!stream)
                {
                    stream.close();
                    std::error_code ignored;
                    std::filesystem::remove(temporary, ignored);
                    return std::unexpected("can't write it");
                }
            }
#endif
            std::error_code error;
            std::filesystem::rename(temporary, target, error);
            if (error)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return std::unexpected("can't replace it with the new version: " + error.message());
            }
            return {};
        }
    }

    std::expected<NewJoinToken, std::string> GenerateJoinToken(const std::string_view user, const N2Net::JoinRole role)
    {
        auto made = MakeToken(user);
        if (!made)
        {
            return std::unexpected(std::move(made.error()));
        }
        NewJoinToken result;
        result.token = std::move(made->token);
        result.entryLine = EntryLine(user, role, made->hash);
        return result;
    }

    std::string FormatTokensFile(const N2Net::JoinTokenTable &table, const N2Net::JoinTokenEntry &added)
    {
        std::string text = "[\n";
        for (const N2Net::JoinTokenEntry &entry : table.Entries())
        {
            text += "  " + EntryLine(entry.user, entry.role, entry.hash) + ",\n";
        }
        text += "  " + EntryLine(added.user, added.role, added.hash) + "\n]\n";
        return text;
    }

    std::expected<NewJoinToken, std::string> AddJoinToken(const std::filesystem::path &path, const std::string_view user,
                                                          const N2Net::JoinRole role)
    {
        const std::string name = path.string();
        if (path.empty())
        {
            return std::unexpected("no tokens file given");
        }

        // The file as the server would read it: an existing one must pass every check (and a link is followed to its
        // target, which is where the new version goes), a missing one is a table with no users
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error)
        {
            return std::unexpected(std::format("tokens file {}: {}", name, error.message()));
        }
        std::filesystem::path target = path;
        std::expected<N2Net::JoinTokenTable, std::string> table = N2Net::JoinTokenTable::Parse("[]");
        if (exists)
        {
            table = N2Net::JoinTokenTable::Load(path);
            if (!table)
            {
                return std::unexpected(std::move(table.error()));
            }
            target = std::filesystem::canonical(path, error);
            if (error)
            {
                return std::unexpected(std::format("tokens file {}: {}", name, error.message()));
            }
        }
        else
        {
            std::filesystem::path folder = path.parent_path();
            if (folder.empty())
            {
                folder = ".";
            }
            if (!std::filesystem::is_directory(folder, error))
            {
                return std::unexpected(std::format("tokens file {}: its folder doesn't exist", name));
            }
        }
        if (!table)
        {
            return std::unexpected(std::format("tokens file {}: {}", name, table.error()));
        }
        if (table->Find(user) != nullptr)
        {
            return std::unexpected(std::format("tokens file {}: the user '{}' is already in it (edit the file to revoke "
                                               "or replace it)",
                                               name, user));
        }

        auto made = MakeToken(user);
        if (!made)
        {
            return std::unexpected(std::move(made.error()));
        }
        const N2Net::JoinTokenEntry entry{std::string(user), role, made->hash};
        const std::string text = FormatTokensFile(*table, entry);
        // What is about to be written must be a file the server accepts, whatever the code above did
        if (const auto parsed = N2Net::JoinTokenTable::Parse(text); !parsed)
        {
            Security::SecureZero(made->token.data(), made->token.size());
            return std::unexpected(std::format("tokens file {}: the new version would not be valid: {}", name,
                                               parsed.error()));
        }
        if (const auto written = WriteFileAtomically(target, text); !written)
        {
            Security::SecureZero(made->token.data(), made->token.size());
            return std::unexpected(std::format("tokens file {}: {}", name, written.error()));
        }

        NewJoinToken result;
        result.token = std::move(made->token);
        result.entryLine = EntryLine(user, role, made->hash);
        return result;
    }

    std::string FormatNewTokenOutput(const std::string_view user, const N2Net::JoinRole role, const NewJoinToken &token,
                                     const std::filesystem::path &path)
    {
        return std::format("New join token for {} ({}). It is shown once: the tokens file keeps only its SHA-256, so "
                           "store the token now.\n"
                           "token: {}\n"
                           "Added {} to {}. A running server reads the file again on SIGHUP (POSIX); a server that is "
                           "started reads it at start.\n",
                           user, N2Net::JoinRoleName(role), token.token, user, path.string());
    }

    int RunNewToken(const ServerOptions &options)
    {
        const std::filesystem::path path(options.tokensFile);
        const auto made = AddJoinToken(path, options.newTokenUser, options.newTokenRole);
        if (!made)
        {
            std::println(stderr, "N2Server --new-token: {}", made.error());
            return 1;
        }
        std::print(stdout, "{}", FormatNewTokenOutput(options.newTokenUser, options.newTokenRole, *made, path));
        std::fflush(stdout);
        return 0;
    }
}
