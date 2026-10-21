#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include "engine/network/n2net/JoinTokens.hpp"
#include "engine/security/Crypto.hpp"
#include "engine/security/TokenTable.hpp"
#include "n2server/ServerOptions.hpp"
#include "n2server/TokenCli.hpp"

using namespace N2Engine;
using namespace N2Engine::Server;
namespace fs = std::filesystem;

// The join-token generator (N2Server --new-token): what it makes, what it writes to the tokens file, what it refuses,
// and that the secret is shown once and kept nowhere. The tokens file is read back with the server's own parser
// (N2Net::JoinTokenTable), so a file the generator writes is a file the server accepts.

namespace
{
    /// A folder of its own under the system's temp folder, removed afterwards
    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _path = fs::temp_directory_path() / ("n2server-tokens-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_path);
        }

        ~TempFolder()
        {
            std::error_code ignored;
            // A test may have made a file unreadable to itself
            for (const auto &entry : fs::directory_iterator(_path, ignored))
            {
                fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                                ignored);
            }
            fs::remove_all(_path, ignored);
        }

        TempFolder(const TempFolder &) = delete;
        TempFolder &operator=(const TempFolder &) = delete;

        [[nodiscard]] fs::path File(const std::string &name) const { return _path / name; }
        [[nodiscard]] const fs::path &Path() const { return _path; }

    private:
        fs::path _path;
    };

    std::string ReadAll(const fs::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }

    void WriteAll(const fs::path &path, const std::string &text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    bool IsLowerHex(const std::string &text)
    {
        for (const char c : text)
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            {
                return false;
            }
        }
        return true;
    }

    /// The user and role the table gives a token (the way the server's handshake finds them: by the token's hash)
    std::optional<N2Net::JoinTokenEntry> Authenticate(const N2Net::JoinTokenTable &table, const std::string &token)
    {
        const auto hash = N2Net::HashJoinToken(token);
        EXPECT_TRUE(hash.has_value());
        if (!hash)
        {
            return std::nullopt;
        }
        return table.Authenticate(*hash, std::nullopt);
    }

    N2Net::JoinTokenTable Load(const fs::path &path)
    {
        auto table = N2Net::JoinTokenTable::Load(path);
        EXPECT_TRUE(table.has_value()) << (table ? "" : table.error());
        return table ? std::move(*table) : *N2Net::JoinTokenTable::Parse("[]");
    }

#if !defined(_WIN32)
    bool PrivateToOwner(const fs::path &path)
    {
        const fs::perms permissions = fs::status(path).permissions();
        return (permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none;
    }
#endif
}

TEST(N2ServerTokenCliTest, ATokenIs256RandomBitsAsLowercaseHexAndItsEntryIsTheHash)
{
    const auto made = GenerateJoinToken("alice", N2Net::JoinRole::Player);
    ASSERT_TRUE(made.has_value()) << made.error();
    EXPECT_EQ(made->token.size(), 64u);
    EXPECT_TRUE(IsLowerHex(made->token));

    // The entry is what the tokens file holds: it parses, names the user and role, and its hash is the token's SHA-256
    const auto table = N2Net::JoinTokenTable::Parse("[" + made->entryLine + "]");
    ASSERT_TRUE(table.has_value()) << table.error();
    ASSERT_EQ(table->Size(), 1u);
    EXPECT_EQ(table->Entries()[0].user, "alice");
    EXPECT_EQ(table->Entries()[0].role, N2Net::JoinRole::Player);
    const auto expected = N2Net::HashJoinToken(made->token);
    ASSERT_TRUE(expected.has_value());
    EXPECT_TRUE(Security::TokenHashesEqual(table->Entries()[0].hash, *expected));
    EXPECT_NE(made->entryLine.find("sha256:"), std::string::npos);

    // The secret is not in the entry, in any spelling of its text
    EXPECT_EQ(made->entryLine.find(made->token), std::string::npos);
}

TEST(N2ServerTokenCliTest, EveryTokenIsDifferent)
{
    std::set<std::string> tokens;
    for (int i = 0; i < 50; ++i)
    {
        const auto made = GenerateJoinToken("bot", N2Net::JoinRole::Spectator);
        ASSERT_TRUE(made.has_value()) << made.error();
        tokens.insert(made->token);
    }
    EXPECT_EQ(tokens.size(), 50u);
}

TEST(N2ServerTokenCliTest, EveryRoleIsWrittenTheWayTheServerReadsIt)
{
    for (const N2Net::JoinRole role : {N2Net::JoinRole::Player, N2Net::JoinRole::Spectator, N2Net::JoinRole::Admin})
    {
        const auto made = GenerateJoinToken("someone", role);
        ASSERT_TRUE(made.has_value()) << made.error();
        EXPECT_NE(made->entryLine.find(std::string("\"role\":\"") + std::string(N2Net::JoinRoleName(role)) + "\""),
                  std::string::npos);
        const auto table = N2Net::JoinTokenTable::Parse("[" + made->entryLine + "]");
        ASSERT_TRUE(table.has_value()) << table.error();
        EXPECT_EQ(table->Entries()[0].role, role);
    }
}

TEST(N2ServerTokenCliTest, AnInvalidUserNameIsRefused)
{
    for (const char *user : {"", "al ice", "alice\n", "al\"ice", "a/b", "caf\xC3\xA9"})
    {
        const auto made = GenerateJoinToken(user, N2Net::JoinRole::Player);
        ASSERT_FALSE(made.has_value()) << "[" << user << "]";
        EXPECT_NE(made.error().find("the user must be"), std::string::npos);
    }
    EXPECT_FALSE(GenerateJoinToken(std::string(Security::MaxTokenUserLength + 1, 'a'), N2Net::JoinRole::Player).has_value());
    EXPECT_TRUE(GenerateJoinToken(std::string(Security::MaxTokenUserLength, 'a'), N2Net::JoinRole::Player).has_value());
    EXPECT_TRUE(GenerateJoinToken("a.b_c@d-e", N2Net::JoinRole::Player).has_value());
}

TEST(N2ServerTokenCliTest, FormatTokensFileWritesOneEntryPerLine)
{
    const auto empty = N2Net::JoinTokenTable::Parse("[]");
    ASSERT_TRUE(empty.has_value());
    Security::TokenHash hashA{};
    hashA.fill(0xAB);
    Security::TokenHash hashC{};
    hashC.fill(0xCD);
    std::string hexA;
    std::string hexC;
    for (int i = 0; i < 32; ++i)
    {
        hexA += "ab";
        hexC += "cd";
    }

    const N2Net::JoinTokenEntry alice{"alice", N2Net::JoinRole::Admin, hashA};
    const std::string one = FormatTokensFile(*empty, alice);
    EXPECT_EQ(one, "[
  {\"user\":\"alice\",\"role\":\"admin\",\"hash\":\"sha256:" + hexA + "\"}
]
");

    const auto parsed = N2Net::JoinTokenTable::Parse(one);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    const N2Net::JoinTokenEntry bob{"bob", N2Net::JoinRole::Spectator, hashC};
    const std::string two = FormatTokensFile(*parsed, bob);
    EXPECT_EQ(two,
              "[
  {\"user\":\"alice\",\"role\":\"admin\",\"hash\":\"sha256:" + hexA + "\"},
"
              "  {\"user\":\"bob\",\"role\":\"spectator\",\"hash\":\"sha256:" + hexC + "\"}
]
");
    EXPECT_TRUE(N2Net::JoinTokenTable::Parse(two).has_value());

    // The same hash for two users is refused by the parser, which is why AddJoinToken parses what it is about to write
    const N2Net::JoinTokenEntry clash{"carol", N2Net::JoinRole::Player, hashA};
    EXPECT_FALSE(N2Net::JoinTokenTable::Parse(FormatTokensFile(*parsed, clash)).has_value());
}

TEST(N2ServerTokenCliTest, AddingToAMissingFileCreatesIt)
{
    const TempFolder folder;
    const fs::path file = folder.File("tokens.json");
    const auto made = AddJoinToken(file, "alice", N2Net::JoinRole::Player);
    ASSERT_TRUE(made.has_value()) << made.error();
    ASSERT_TRUE(fs::is_regular_file(file));

    // The server's own loader accepts it (a regular file, private to its owner on POSIX, well formed)
    const N2Net::JoinTokenTable table = Load(file);
    ASSERT_EQ(table.Size(), 1u);
    const auto entry = Authenticate(table, made->token);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->user, "alice");
    EXPECT_EQ(entry->role, N2Net::JoinRole::Player);
    EXPECT_FALSE(Authenticate(table, std::string(64, '0')).has_value());

#if !defined(_WIN32)
    EXPECT_TRUE(PrivateToOwner(file)) << "a tokens file must not be readable by group or others";
#endif

    // No temporary file is left beside it
    int files = 0;
    for (const auto &item : fs::directory_iterator(folder.Path()))
    {
        (void)item;
        ++files;
    }
    EXPECT_EQ(files, 1);
}

TEST(N2ServerTokenCliTest, TheFileKeepsTheHashAndNeverTheToken)
{
    const TempFolder folder;
    const fs::path file = folder.File("tokens.json");
    const auto made = AddJoinToken(file, "alice", N2Net::JoinRole::Admin);
    ASSERT_TRUE(made.has_value()) << made.error();

    const std::string text = ReadAll(file);
    EXPECT_EQ(text.find(made->token), std::string::npos) << "the secret is never written";
    const auto hash = N2Net::HashJoinToken(made->token);
    ASSERT_TRUE(hash.has_value());
    EXPECT_NE(text.find(Security::FormatTokenHash(*hash)), std::string::npos);
    EXPECT_NE(text.find("\"role\":\"admin\""), std::string::npos);
}

TEST(N2ServerTokenCliTest, AddingAppendsAndKeepsEveryOtherUser)
{
    const TempFolder folder;
    const fs::path file = folder.File("tokens.json");
    const auto alice = AddJoinToken(file, "alice", N2Net::JoinRole::Player);
    const auto bob = AddJoinToken(file, "bob", N2Net::JoinRole::Spectator);
    const auto carol = AddJoinToken(file, "carol", N2Net::JoinRole::Admin);
    ASSERT_TRUE(alice.has_value()) << alice.error();
    ASSERT_TRUE(bob.has_value()) << bob.error();
    ASSERT_TRUE(carol.has_value()) << carol.error();

    const N2Net::JoinTokenTable table = Load(file);
    ASSERT_EQ(table.Size(), 3u);
    EXPECT_EQ(table.Entries()[0].user, "alice");
    EXPECT_EQ(table.Entries()[1].user, "bob");
    EXPECT_EQ(table.Entries()[2].user, "carol");
    const auto a = Authenticate(table, alice->token);
    const auto b = Authenticate(table, bob->token);
    const auto c = Authenticate(table, carol->token);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(a->user, "alice");
    EXPECT_EQ(a->role, N2Net::JoinRole::Player);
    EXPECT_EQ(b->user, "bob");
    EXPECT_EQ(b->role, N2Net::JoinRole::Spectator);
    EXPECT_EQ(c->user, "carol");
    EXPECT_EQ(c->role, N2Net::JoinRole::Admin);
    // Each token opens only its own user
    EXPECT_NE(alice->token, bob->token);
    EXPECT_NE(bob->token, carol->token);

#if !defined(_WIN32)
    EXPECT_TRUE(PrivateToOwner(file));
#endif
}

TEST(N2ServerTokenCliTest, AUserAlreadyInTheFileIsRefusedAndTheFileIsUntouched)
{
    const TempFolder folder;
    const fs::path file = folder.File("tokens.json");
    ASSERT_TRUE(AddJoinToken(file, "alice", N2Net::JoinRole::Player).has_value());
    const std::string before = ReadAll(file);

    const auto again = AddJoinToken(file, "alice", N2Net::JoinRole::Admin);
    ASSERT_FALSE(again.has_value());
    EXPECT_NE(again.error().find("'alice' is already in it"), std::string::npos);
    EXPECT_EQ(ReadAll(file), before);
}

TEST(N2ServerTokenCliTest, AnInvalidUserCreatesNoFile)
{
    const TempFolder folder;
    const fs::path file = folder.File("tokens.json");
    const auto made = AddJoinToken(file, "not valid", N2Net::JoinRole::Player);
    ASSERT_FALSE(made.has_value());
    EXPECT_NE(made.error().find("the user must be"), std::string::npos);
    EXPECT_FALSE(fs::exists(file));
}

TEST(N2ServerTokenCliTest, AFolderThatDoesNotExistIsAnError)
{
    const TempFolder folder;
    const fs::path file = folder.File("missing") / "tokens.json";
    const auto made = AddJoinToken(file, "alice", N2Net::JoinRole::Player);
    ASSERT_FALSE(made.has_value());
    EXPECT_NE(made.error().find("its folder doesn't exist"), std::string::npos);
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(fs::exists(folder.File("missing")));
}

TEST(N2ServerTokenCliTest, AnEmptyPathIsAnError)
{
    const auto made = AddJoinToken(fs::path(), "alice", N2Net::JoinRole::Player);
    ASSERT_FALSE(made.has_value());
    EXPECT_NE(made.error().find("no tokens file"), std::string::npos);
}

TEST(N2ServerTokenCliTest, AFileTheServerWouldRefuseIsRefusedAndLeftAlone)
{
    const TempFolder folder;
    for (const std::string &text : {std::string("not json"), std::string("{\"user\":\"alice\"}"), std::string("[1,2]"),
                                    std::string("[{\"user\":\"alice\",\"role\":\"editor\",\"hash\":\"sha256:" +
                                                std::string(64, '0') + "\"}]")})
    {
        const fs::path file = folder.File("bad.json");
        WriteAll(file, text);
#if !defined(_WIN32)
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
        const auto made = AddJoinToken(file, "bob", N2Net::JoinRole::Player);
        ASSERT_FALSE(made.has_value()) << text;
        EXPECT_NE(made.error().find("tokens file"), std::string::npos);
        EXPECT_EQ(ReadAll(file), text) << "a file that is refused is not rewritten";
    }
}

#if !defined(_WIN32)
TEST(N2ServerTokenCliTest, AFileOtherUsersCanReadIsRefusedAndLeftAlone)
{
    const TempFolder folder;
    const fs::path file = folder.File("tokens.json");
    ASSERT_TRUE(AddJoinToken(file, "alice", N2Net::JoinRole::Player).has_value());
    const std::string before = ReadAll(file);
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read,
                    fs::perm_options::replace);

    const auto made = AddJoinToken(file, "bob", N2Net::JoinRole::Player);
    ASSERT_FALSE(made.has_value());
    EXPECT_NE(made.error().find("tokens file"), std::string::npos);
    EXPECT_EQ(ReadAll(file), before);
}

TEST(N2ServerTokenCliTest, ASymbolicLinkIsFollowedAndTheLinkKept)
{
    const TempFolder folder;
    const fs::path target = folder.File("real.json");
    const fs::path link = folder.File("link.json");
    ASSERT_TRUE(AddJoinToken(target, "alice", N2Net::JoinRole::Player).has_value());
    std::error_code error;
    fs::create_symlink(target, link, error);
    if (error)
    {
        GTEST_SKIP() << "no symbolic links here: " << error.message();
    }

    const auto made = AddJoinToken(link, "bob", N2Net::JoinRole::Player);
    ASSERT_TRUE(made.has_value()) << made.error();
    EXPECT_TRUE(fs::is_symlink(link)) << "the link is not replaced by a file";
    EXPECT_EQ(Load(target).Size(), 2u);
}
#endif

TEST(N2ServerTokenCliTest, TheOutputShowsTheTokenOnceOnALineOfItsOwnAndNeverTheHash)
{
    const auto made = GenerateJoinToken("alice", N2Net::JoinRole::Player);
    ASSERT_TRUE(made.has_value()) << made.error();
    const std::string output = FormatNewTokenOutput("alice", N2Net::JoinRole::Player, *made, fs::path("tokens.json"));

    // A launcher takes the token from the line that starts "token: "
    const std::string line = "token: " + made->token + "\n";
    const std::size_t at = output.find(line);
    ASSERT_NE(at, std::string::npos);
    EXPECT_TRUE(at == 0 || output[at - 1] == '\n') << "on a line of its own";
    EXPECT_EQ(output.find(made->token, at + line.size()), std::string::npos) << "once";
    EXPECT_EQ(output.find("sha256:"), std::string::npos);
    EXPECT_NE(output.find("alice"), std::string::npos);
    EXPECT_NE(output.find("player"), std::string::npos);
    EXPECT_NE(output.find("tokens.json"), std::string::npos);
}

TEST(N2ServerTokenCliTest, RunNewTokenPrintsTheTokenToStdoutAndAddsTheHash)
{
    const TempFolder folder;
    ServerOptions options;
    options.newTokenUser = "alice";
    options.newTokenRole = N2Net::JoinRole::Spectator;
    options.tokensFile = folder.File("tokens.json").string();

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const int code = RunNewToken(options);
    const std::string out = testing::internal::GetCapturedStdout();
    const std::string err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(code, 0);
    EXPECT_TRUE(err.empty()) << err;
    const std::size_t start = out.find("token: ");
    ASSERT_NE(start, std::string::npos);
    const std::string token = out.substr(start + 7, 64);
    EXPECT_TRUE(IsLowerHex(token));
    EXPECT_EQ(token.size(), 64u);

    const N2Net::JoinTokenTable table = Load(options.tokensFile);
    const auto entry = Authenticate(table, token);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->user, "alice");
    EXPECT_EQ(entry->role, N2Net::JoinRole::Spectator);
    EXPECT_EQ(ReadAll(options.tokensFile).find(token), std::string::npos);
}

TEST(N2ServerTokenCliTest, RunNewTokenReportsAFailureOnStderrAndPrintsNoToken)
{
    const TempFolder folder;
    ServerOptions options;
    options.newTokenUser = "alice";
    options.tokensFile = (folder.Path() / "no-such-folder" / "tokens.json").string();

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const int code = RunNewToken(options);
    const std::string out = testing::internal::GetCapturedStdout();
    const std::string err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(code, 1);
    EXPECT_TRUE(out.empty()) << out;
    EXPECT_NE(err.find("N2Server --new-token: "), std::string::npos);
}
