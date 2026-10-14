#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <editor-server/EditorTls.hpp>
#include <editor-server/TokenTable.hpp>

using namespace N2Engine::Editor;
namespace fs = std::filesystem;

// The per-user tokens file (--tokens-file) as plain logic, with no sockets: the table, its parser, the file checks and
// the SHA-256 it depends on. Only the tests that hash a token (a TLS build, N2ENGINE_EDITOR_TLS) skip themselves
// elsewhere; the parser, the matching (on digests) and the file checks run on every leg. EditorTokensTests.cpp is the
// same feature over the server.

namespace
{
    /// A digest whose every byte is `fill`: a stand-in for a hash, where the test isn't about SHA-256
    TokenHash Digest(const unsigned char fill)
    {
        TokenHash hash{};
        hash.fill(fill);
        return hash;
    }

    std::string Entry(const std::string &user, const std::string &role, const TokenHash &hash)
    {
        return "{\"user\":\"" + user + "\",\"role\":\"" + role + "\",\"hash\":\"" + FormatTokenHash(hash) + "\"}";
    }

    std::string HexOf(const TokenHash &hash)
    {
        return FormatTokenHash(hash).substr(TokenHashPrefix.size());
    }

    /// A folder of its own under the system's temp folder, removed afterwards
    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _path = fs::temp_directory_path() /
                    ("n2-tokens-table-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_path);
        }

        ~TempFolder()
        {
            std::error_code error;
            fs::remove_all(_path, error);
        }

        TempFolder(const TempFolder &) = delete;
        TempFolder &operator=(const TempFolder &) = delete;

        [[nodiscard]] const fs::path &Path() const { return _path; }

    private:
        fs::path _path;
    };

    void WriteFile(const fs::path &path, const std::string &text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

#ifndef _WIN32
    void MakePrivate(const fs::path &path)
    {
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    }
#else
    void MakePrivate(const fs::path &) {}
#endif

    std::string HexString(const std::array<unsigned char, 32> &digest)
    {
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string text;
        for (const unsigned char byte : digest)
        {
            text += digits[byte >> 4];
            text += digits[byte & 0x0F];
        }
        return text;
    }
}

// ==================== Roles, names and hashes ====================

TEST(TokenTableTest, RolesAreSpelledInLowercaseAndNothingElse)
{
    EXPECT_EQ(ParseTokenRole("admin"), TokenRole::Admin);
    EXPECT_EQ(ParseTokenRole("editor"), TokenRole::Editor);
    EXPECT_EQ(ParseTokenRole("viewer"), TokenRole::Viewer);
    for (const char *wrong : {"", "Admin", "EDITOR", " viewer", "viewer ", "root", "readonly", "admin\n"})
    {
        EXPECT_FALSE(ParseTokenRole(wrong).has_value()) << "'" << wrong << "'";
    }
    EXPECT_EQ(TokenRoleName(TokenRole::Admin), "admin");
    EXPECT_EQ(TokenRoleName(TokenRole::Editor), "editor");
    EXPECT_EQ(TokenRoleName(TokenRole::Viewer), "viewer");
    // The role of a name is the name's role
    for (const TokenRole role : {TokenRole::Viewer, TokenRole::Editor, TokenRole::Admin})
    {
        EXPECT_EQ(ParseTokenRole(TokenRoleName(role)), role);
    }
}

TEST(TokenTableTest, UserNamesAreShortAndLogSafe)
{
    for (const char *valid : {"alice", "Bob", "a", "ci-runner", "build.agent_2", "n.cerisano@method.me", "0"})
    {
        EXPECT_TRUE(IsValidTokenUserName(valid)) << valid;
    }
    EXPECT_TRUE(IsValidTokenUserName(std::string(MaxTokenUserLength, 'x')));
    EXPECT_FALSE(IsValidTokenUserName(std::string(MaxTokenUserLength + 1, 'x')));
    for (const char *invalid : {"", " ", "a b", "a\"b", "a\\b", "a/b", "a:b", "a,b", "a=b", "a\nb", "caf\xC3\xA9"})
    {
        EXPECT_FALSE(IsValidTokenUserName(invalid)) << invalid;
    }
    EXPECT_FALSE(IsValidTokenUserName(std::string_view("a\0b", 3)));
}

TEST(TokenTableTest, AHashIsSha256AndSixtyFourLowercaseHexDigits)
{
    TokenHash hash{};
    for (std::size_t i = 0; i < hash.size(); ++i)
    {
        hash[i] = static_cast<unsigned char>(i * 7 + 1);
    }
    const std::string text = FormatTokenHash(hash);
    EXPECT_EQ(text.size(), TokenHashPrefix.size() + 64);
    EXPECT_EQ(text.rfind("sha256:", 0), 0u);
    EXPECT_EQ(ParseTokenHash(text), hash);

    const std::string digits = text.substr(TokenHashPrefix.size());
    EXPECT_FALSE(ParseTokenHash(digits).has_value()) << "no prefix";
    EXPECT_FALSE(ParseTokenHash("sha1:" + digits).has_value()) << "another prefix";
    EXPECT_FALSE(ParseTokenHash("SHA256:" + digits).has_value()) << "an uppercase prefix";
    EXPECT_FALSE(ParseTokenHash(text.substr(0, text.size() - 1)).has_value()) << "short";
    EXPECT_FALSE(ParseTokenHash(text + "0").has_value()) << "long";
    EXPECT_FALSE(ParseTokenHash("sha256:").has_value()) << "empty";
    std::string upper = text;
    for (char &c : upper)
    {
        if (c >= 'a' && c <= 'f')
            c = static_cast<char>(c - 'a' + 'A');
    }
    EXPECT_FALSE(ParseTokenHash(upper).has_value()) << "uppercase hex is not what --new-token writes";
    std::string notHex = text;
    notHex.back() = 'g';
    EXPECT_FALSE(ParseTokenHash(notHex).has_value());
    notHex.back() = ' ';
    EXPECT_FALSE(ParseTokenHash(notHex).has_value());
}

TEST(TokenTableTest, HashesAreEqualOnlyWhenEveryByteIs)
{
    const TokenHash a = Digest(0x11);
    EXPECT_TRUE(TokenHashesEqual(a, Digest(0x11)));
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        TokenHash other = a;
        other[i] ^= 0x01; // a difference in any one byte, first or last
        EXPECT_FALSE(TokenHashesEqual(a, other)) << "byte " << i;
    }
}

// ==================== Parsing ====================

TEST(TokenTableTest, ParsesAFileOfUsers)
{
    const std::string text = "[" + Entry("alice", "admin", Digest(1)) + ",\n " + Entry("bob", "editor", Digest(2)) + ",\n " +
                             Entry("carol", "viewer", Digest(3)) + "]";
    const auto table = TokenTable::Parse(text);
    ASSERT_TRUE(table) << table.error();
    ASSERT_EQ(table->Size(), 3u);
    EXPECT_EQ(table->Entries()[0].user, "alice");
    EXPECT_EQ(table->Entries()[0].role, TokenRole::Admin);
    EXPECT_EQ(table->Entries()[0].hash, Digest(1));
    EXPECT_EQ(table->Entries()[1].user, "bob");
    EXPECT_EQ(table->Entries()[1].role, TokenRole::Editor);
    EXPECT_EQ(table->Entries()[2].user, "carol");
    EXPECT_EQ(table->Entries()[2].role, TokenRole::Viewer);

    ASSERT_NE(table->Find("bob"), nullptr);
    EXPECT_EQ(table->Find("bob")->role, TokenRole::Editor);
    EXPECT_EQ(table->Find("dave"), nullptr);
    EXPECT_EQ(table->Find("Bob"), nullptr) << "user names are case sensitive";
    EXPECT_EQ(table->Find(""), nullptr);
}

TEST(TokenTableTest, AnEmptyFileHasNoUsersAndThatIsHowEveryoneIsRevoked)
{
    const auto table = TokenTable::Parse("[]");
    ASSERT_TRUE(table) << table.error();
    EXPECT_EQ(table->Size(), 0u);
    EXPECT_FALSE(table->Authenticate(Digest(1), std::nullopt).has_value());

    // Whitespace around it, and a byte order mark an editor on Windows may add
    EXPECT_TRUE(TokenTable::Parse("  \n[ ]\n"));
    const auto withBom = TokenTable::Parse(std::string("\xEF\xBB\xBF") + "[" + Entry("alice", "admin", Digest(1)) + "]");
    ASSERT_TRUE(withBom) << withBom.error();
    EXPECT_EQ(withBom->Size(), 1u);
}

TEST(TokenTableTest, MalformedFilesAreRefusedWithAClearErrorThatNeverRepeatsAHash)
{
    const TokenHash good = Digest(0xAB);
    const std::string goodHash = FormatTokenHash(good);
    const std::string goodDigits = HexOf(good);
    auto entry = [&](const std::string &user, const std::string &role, const std::string &hash)
    {
        return "{\"user\":\"" + user + "\",\"role\":\"" + role + "\",\"hash\":\"" + hash + "\"}";
    };
    struct Case
    {
        const char *what;
        std::string text;
        std::string errorContains;
    };
    const std::vector<Case> cases = {
        {"not JSON", "alice admin", "not valid JSON"},
        {"truncated JSON", "[" + entry("alice", "admin", goodHash), "not valid JSON"},
        {"an object, not an array", "{\"user\":\"alice\"}", "must be a JSON array"},
        {"a string, not an array", "\"[]\"", "must be a JSON array"},
        {"an entry that is not an object", "[\"alice\"]", "entry 1"},
        {"a missing hash", "[{\"user\":\"alice\",\"role\":\"admin\"}]", "entry 1"},
        {"a missing role", "[{\"user\":\"alice\",\"hash\":\"" + goodHash + "\"}]", "entry 1"},
        {"a missing user", "[{\"role\":\"admin\",\"hash\":\"" + goodHash + "\"}]", "entry 1"},
        {"an unknown key", "[{\"user\":\"alice\",\"role\":\"admin\",\"hash\":\"" + goodHash + "\",\"note\":\"x\"}]",
         "only user, role and hash"},
        {"a role that is not a string", "[{\"user\":\"alice\",\"role\":1,\"hash\":\"" + goodHash + "\"}]", "strings"},
        {"a bad role", "[" + entry("alice", "root", goodHash) + "]", "admin, editor or viewer"},
        {"an uppercase role", "[" + entry("alice", "Admin", goodHash) + "]", "admin, editor or viewer"},
        {"an empty user", "[" + entry("", "admin", goodHash) + "]", "entry 1: the user"},
        {"a user with a space", "[" + entry("al ice", "admin", goodHash) + "]", "entry 1: the user"},
        {"a short hash", "[" + entry("alice", "admin", goodHash.substr(0, goodHash.size() - 2)) + "]",
         "64 lowercase hex"},
        {"a long hash", "[" + entry("alice", "admin", goodHash + "00") + "]", "64 lowercase hex"},
        {"a hash without the prefix", "[" + entry("alice", "admin", goodDigits) + "]", "64 lowercase hex"},
        {"a hash that is not hex", "[" + entry("alice", "admin", "sha256:" + std::string(64, 'z')) + "]",
         "64 lowercase hex"},
        {"an uppercase hash", "[" + entry("alice", "admin", "sha256:" + std::string(64, 'A')) + "]", "64 lowercase hex"},
        {"a plaintext token", "[" + entry("alice", "admin", "correct horse battery staple") + "]", "--new-token"},
        {"a user listed twice",
         "[" + Entry("alice", "admin", Digest(1)) + "," + Entry("alice", "viewer", Digest(2)) + "]", "listed twice"},
        {"one token for two users",
         "[" + Entry("alice", "admin", Digest(1)) + "," + Entry("bob", "viewer", Digest(1)) + "]", "same hash"},
        {"a second entry that is bad",
         "[" + Entry("alice", "admin", Digest(1)) + "," + entry("bob", "nobody", goodHash) + "]", "entry 2 (bob)"},
    };
    for (const Case &testCase : cases)
    {
        const auto table = TokenTable::Parse(testCase.text);
        ASSERT_FALSE(table) << testCase.what;
        EXPECT_NE(table.error().find(testCase.errorContains), std::string::npos)
            << testCase.what << ": " << table.error();
        // Safe to log: neither a hash nor the file's text comes back in it
        EXPECT_EQ(table.error().find(goodDigits), std::string::npos) << testCase.what << ": " << table.error();
        EXPECT_EQ(table.error().find("correct horse"), std::string::npos) << testCase.what << ": " << table.error();
        EXPECT_EQ(table.error().find(HexOf(Digest(1))), std::string::npos) << testCase.what << ": " << table.error();
    }
}

// ==================== Matching ====================

TEST(TokenTableTest, ATokenMatchesTheUserWhoseHashItIs)
{
    const auto table = TokenTable::Parse("[" + Entry("alice", "admin", Digest(1)) + "," + Entry("bob", "editor", Digest(2)) +
                                         "," + Entry("carol", "viewer", Digest(3)) + "]");
    ASSERT_TRUE(table) << table.error();

    // Without a user (a client from before 1.12.0) the token is matched across every entry
    for (const auto &[hash, user, role] :
         {std::tuple{Digest(1), "alice", TokenRole::Admin}, std::tuple{Digest(2), "bob", TokenRole::Editor},
          std::tuple{Digest(3), "carol", TokenRole::Viewer}})
    {
        const auto match = table->Authenticate(hash, std::nullopt);
        ASSERT_TRUE(match.has_value()) << user;
        EXPECT_EQ(match->user, user);
        EXPECT_EQ(match->role, role);
        EXPECT_EQ(match->hash, hash);
    }

    // With a user, only that user's entry is considered
    const auto named = table->Authenticate(Digest(2), std::optional<std::string_view>("bob"));
    ASSERT_TRUE(named.has_value());
    EXPECT_EQ(named->user, "bob");
    EXPECT_EQ(named->role, TokenRole::Editor);
}

TEST(TokenTableTest, AWrongTokenOrAWrongUserMatchesNothing)
{
    const auto table = TokenTable::Parse("[" + Entry("alice", "admin", Digest(1)) + "," + Entry("bob", "editor", Digest(2)) +
                                         "]");
    ASSERT_TRUE(table) << table.error();

    EXPECT_FALSE(table->Authenticate(Digest(9), std::nullopt).has_value()) << "a token nobody has";
    EXPECT_FALSE(table->Authenticate(Digest(9), std::optional<std::string_view>("alice")).has_value());
    // The right token under another user's name: bob's token does not make alice's entry match
    EXPECT_FALSE(table->Authenticate(Digest(2), std::optional<std::string_view>("alice")).has_value());
    EXPECT_FALSE(table->Authenticate(Digest(1), std::optional<std::string_view>("bob")).has_value());
    // A user the file doesn't have, with a token the file does
    EXPECT_FALSE(table->Authenticate(Digest(1), std::optional<std::string_view>("mallory")).has_value());
    EXPECT_FALSE(table->Authenticate(Digest(1), std::optional<std::string_view>("Alice")).has_value());
    // A hash that differs from a real one in its last byte only
    TokenHash nearMiss = Digest(1);
    nearMiss.back() ^= 0x01;
    EXPECT_FALSE(table->Authenticate(nearMiss, std::nullopt).has_value());
    nearMiss = Digest(1);
    nearMiss.front() ^= 0x80;
    EXPECT_FALSE(table->Authenticate(nearMiss, std::nullopt).has_value());
}

TEST(TokenTableTest, TheLastEntryAndTheFirstBothMatch)
{
    // Every entry is compared, none is skipped: a match at either end, and in a long table, is found
    std::string text = "[";
    for (int i = 0; i < 50; ++i)
    {
        text += (i == 0 ? "" : ",") + Entry("user" + std::to_string(i), i % 3 == 0 ? "admin" : "viewer",
                                           Digest(static_cast<unsigned char>(i + 1)));
    }
    text += "]";
    const auto table = TokenTable::Parse(text);
    ASSERT_TRUE(table) << table.error();
    ASSERT_EQ(table->Size(), 50u);
    for (const int i : {0, 1, 24, 48, 49})
    {
        const auto match = table->Authenticate(Digest(static_cast<unsigned char>(i + 1)), std::nullopt);
        ASSERT_TRUE(match.has_value()) << i;
        EXPECT_EQ(match->user, "user" + std::to_string(i));
    }
}

// ==================== The file ====================

TEST(TokensFileTest, LoadsAFileOfUsers)
{
    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    WriteFile(file, "[" + Entry("alice", "admin", Digest(1)) + "]");
    MakePrivate(file);

    const auto table = LoadTokensFile(file);
    ASSERT_TRUE(table) << table.error();
    EXPECT_EQ(table->Size(), 1u);
}

TEST(TokensFileTest, AMissingFileOrAFolderIsAnErrorNamingIt)
{
    TempFolder folder;
    const auto missing = LoadTokensFile(folder.Path() / "nope.json");
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("nope.json"), std::string::npos) << missing.error();

    const auto directory = LoadTokensFile(folder.Path());
    ASSERT_FALSE(directory);
}

TEST(TokensFileTest, AMalformedFileIsAnErrorNamingTheFileAndTheEntry)
{
    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    WriteFile(file, "[" + Entry("alice", "admin", Digest(1)) + ",{\"user\":\"bob\",\"role\":\"admin\",\"hash\":\"nope\"}]");
    MakePrivate(file);
    const auto table = LoadTokensFile(file);
    ASSERT_FALSE(table);
    EXPECT_NE(table.error().find("tokens.json"), std::string::npos) << table.error();
    EXPECT_NE(table.error().find("entry 2 (bob)"), std::string::npos) << table.error();
}

TEST(TokensFileTest, AFileLargerThanTheLimitIsRefusedUnread)
{
    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    WriteFile(file, "[" + std::string(MaxTokensFileBytes, ' ') + "]");
    MakePrivate(file);
    const auto table = LoadTokensFile(file);
    ASSERT_FALSE(table);
    EXPECT_NE(table.error().find("larger than"), std::string::npos) << table.error();
}

TEST(TokensFileTest, OnPosixAFileItsGroupOrOthersCanAccessIsRefused)
{
#ifdef _WIN32
    GTEST_SKIP() << "file permissions aren't checked on Windows (ACLs)";
#else
    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    WriteFile(file, "[" + Entry("alice", "admin", Digest(1)) + "]");

    using fs::perms;
    for (const perms bad : {perms::owner_read | perms::owner_write | perms::group_read,
                            perms::owner_read | perms::owner_write | perms::others_read,
                            perms::owner_read | perms::owner_write | perms::group_write,
                            perms::owner_read | perms::owner_write | perms::others_write,
                            perms::owner_all | perms::group_all | perms::others_all})
    {
        fs::permissions(file, bad, fs::perm_options::replace);
        const auto refused = LoadTokensFile(file);
        ASSERT_FALSE(refused) << static_cast<int>(bad);
        EXPECT_NE(refused.error().find("chmod 600"), std::string::npos) << refused.error();
    }

    fs::permissions(file, perms::owner_read | perms::owner_write, fs::perm_options::replace);
    EXPECT_TRUE(LoadTokensFile(file));
    fs::permissions(file, perms::owner_read, fs::perm_options::replace);
    EXPECT_TRUE(LoadTokensFile(file)) << "owner read-only is private too";
#endif
}

TEST(TokensFileTest, TheSignatureChangesWhenTheFileIsRewritten)
{
    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";

    EXPECT_FALSE(StatTokensFile(file).exists);
    WriteFile(file, "[]");
    const TokensFileSignature first = StatTokensFile(file);
    EXPECT_TRUE(first.exists);
    EXPECT_EQ(first.size, 2u);
    EXPECT_EQ(StatTokensFile(file), first) << "reading it doesn't change it";

    // A different size is a different signature, whatever the clock's granularity
    WriteFile(file, "[ ]");
    const TokensFileSignature longer = StatTokensFile(file);
    EXPECT_NE(longer, first);

    // The same size and a later modification time is one too
    WriteFile(file, "[  ]");
    const TokensFileSignature third = StatTokensFile(file);
    WriteFile(file, "[\n]");
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    EXPECT_NE(StatTokensFile(file), third);

    std::error_code error;
    fs::remove(file, error);
    EXPECT_FALSE(StatTokensFile(file).exists);
    EXPECT_NE(StatTokensFile(file), longer);
}

// ==================== SHA-256 and --new-token ====================

TEST(TokenHashTest, Sha256MatchesTheNistVectors)
{
    if (!EditorTlsServer::IsAvailable())
    {
        std::array<unsigned char, 32> digest{};
        EXPECT_FALSE(EditorTlsServer::Sha256("abc", digest)) << "a build without TLS has no SHA-256";
        EXPECT_FALSE(HashToken("abc").has_value());
        GTEST_SKIP() << "this build has no TLS library (N2ENGINE_EDITOR_TLS)";
    }

    struct Vector
    {
        std::string message;
        const char *digest;
    };
    const std::vector<Vector> vectors = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (const Vector &vector : vectors)
    {
        std::array<unsigned char, 32> digest{};
        ASSERT_TRUE(EditorTlsServer::Sha256(vector.message, digest)) << vector.message.size();
        EXPECT_EQ(HexString(digest), vector.digest) << "message of " << vector.message.size() << " bytes";
    }

    // HashToken is the same digest, in the table's type
    const auto hash = HashToken("abc");
    ASSERT_TRUE(hash.has_value());
    EXPECT_EQ(FormatTokenHash(*hash), "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(TokenHashTest, TheRandomGeneratorGivesDifferentBytesEachTime)
{
    if (!EditorTlsServer::IsAvailable())
    {
        std::array<unsigned char, 32> bytes{};
        EXPECT_FALSE(EditorTlsServer::RandomBytes(bytes.data(), bytes.size()));
        GTEST_SKIP() << "this build has no TLS library (N2ENGINE_EDITOR_TLS)";
    }
    std::set<std::string> seen;
    for (int i = 0; i < 8; ++i)
    {
        std::array<unsigned char, 32> bytes{};
        ASSERT_TRUE(EditorTlsServer::RandomBytes(bytes.data(), bytes.size()));
        EXPECT_NE(HexString(bytes), std::string(64, '0'));
        seen.insert(HexString(bytes));
    }
    EXPECT_EQ(seen.size(), 8u);
    // More than the generator's single request limit (1024 bytes) still fills it all
    std::vector<unsigned char> many(3000, 0);
    ASSERT_TRUE(EditorTlsServer::RandomBytes(many.data(), many.size()));
    EXPECT_NE(std::count(many.begin() + 2000, many.end(), 0), 1000) << "the last bytes were filled too";
    EXPECT_TRUE(EditorTlsServer::RandomBytes(nullptr, 0));
}

TEST(NewTokenTest, MakesA256BitTokenAndTheLineThatMatchesIt)
{
    if (!EditorTlsServer::IsAvailable())
    {
        const auto made = GenerateToken("alice", TokenRole::Editor);
        ASSERT_FALSE(made);
        EXPECT_NE(made.error().find("N2ENGINE_EDITOR_TLS"), std::string::npos) << made.error();
        GTEST_SKIP() << "this build has no TLS library (N2ENGINE_EDITOR_TLS)";
    }

    const auto first = GenerateToken("alice", TokenRole::Editor);
    const auto second = GenerateToken("alice", TokenRole::Editor);
    ASSERT_TRUE(first) << first.error();
    ASSERT_TRUE(second) << second.error();
    EXPECT_EQ(first->token.size(), 64u) << "256 bits as hex";
    EXPECT_NE(first->token, second->token);
    for (const char c : first->token)
    {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << first->token;
    }

    // The line parses as a one-entry file whose hash is the SHA-256 of the token's text, for that user and role
    const auto table = TokenTable::Parse("[" + first->entryLine + "]");
    ASSERT_TRUE(table) << table.error();
    ASSERT_EQ(table->Size(), 1u);
    EXPECT_EQ(table->Entries()[0].user, "alice");
    EXPECT_EQ(table->Entries()[0].role, TokenRole::Editor);
    const auto hash = HashToken(first->token);
    ASSERT_TRUE(hash.has_value());
    const auto match = table->Authenticate(*hash, std::optional<std::string_view>("alice"));
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->role, TokenRole::Editor);
    // And the other token is nobody's
    EXPECT_FALSE(table->Authenticate(*HashToken(second->token), std::nullopt).has_value());
    // The line holds the hash, not the token
    EXPECT_EQ(first->entryLine.find(first->token), std::string::npos);
    EXPECT_NE(first->entryLine.find("\"role\":\"editor\""), std::string::npos);

    // What the command prints: the token once, then the line
    const std::string output = FormatNewTokenOutput("alice", TokenRole::Editor, *first);
    EXPECT_NE(output.find("token: " + first->token + "\n"), std::string::npos) << output;
    EXPECT_NE(output.find(first->entryLine + "\n"), std::string::npos) << output;
    EXPECT_EQ(output.find(first->token), output.rfind(first->token)) << "the token appears once";
}

TEST(NewTokenTest, RefusesAnInvalidUser)
{
    for (const char *user : {"", "a b", "a\"b", "caf\xC3\xA9"})
    {
        const auto made = GenerateToken(user, TokenRole::Viewer);
        ASSERT_FALSE(made) << user;
    }
}
