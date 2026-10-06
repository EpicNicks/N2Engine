#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>

#include <engine/io/ResourceUUID.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
namespace fs = std::filesystem;

namespace
{
    /// A real folder (temp/n2-namespace-test/project), so weakly_canonical resolves all of it
    class ResourceUUIDNamespaceTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            s_root = fs::temp_directory_path() / "n2-namespace-test";
            fs::create_directories(s_root / "project");
            s_project = fs::canonical(s_root / "project");
        }

        static void TearDownTestSuite()
        {
            std::error_code error;
            fs::remove_all(s_root, error);
        }

        static inline fs::path s_root;
        static inline fs::path s_project;
    };
}

TEST_F(ResourceUUIDNamespaceTest, IsTheNameBasedUUIDOfTheAbsoluteForwardSlashPath)
{
    // What lua_project hashed before the helper, for its default N2_LUA_PROJECT_DIR (a CMake path)
    const std::string generic = s_project.generic_string();
    EXPECT_EQ(IO::ResourceUUID::NormalizeProjectDir(s_project), generic);
    EXPECT_EQ(IO::ResourceUUID::NamespaceForProjectDir(s_project),
              Math::UUID::GenerateNameBased(Math::UUID::ZERO, generic));
    EXPECT_EQ(generic.find('\\'), std::string::npos);
}

TEST_F(ResourceUUIDNamespaceTest, BackslashAndForwardSlashSpellingsAgree)
{
    std::string forward = s_project.generic_string();
    std::string backward = forward;
#ifdef _WIN32
    for (char &c : backward)
    {
        if (c == '/')
            c = '\\';
    }
#endif
    EXPECT_EQ(IO::ResourceUUID::NamespaceForProjectDir(fs::path(forward)),
              IO::ResourceUUID::NamespaceForProjectDir(fs::path(backward)));
}

TEST_F(ResourceUUIDNamespaceTest, ARelativePathAgreesWithItsAbsoluteForm)
{
    const fs::path previous = fs::current_path();
    fs::current_path(s_root);
    const Math::UUID relative = IO::ResourceUUID::NamespaceForProjectDir("project");
    const Math::UUID dotted = IO::ResourceUUID::NamespaceForProjectDir("./project/../project");
    fs::current_path(previous);

    EXPECT_EQ(relative, IO::ResourceUUID::NamespaceForProjectDir(s_project));
    EXPECT_EQ(dotted, IO::ResourceUUID::NamespaceForProjectDir(s_project));
}

TEST_F(ResourceUUIDNamespaceTest, ATrailingSlashDoesNotChangeIt)
{
    EXPECT_EQ(IO::ResourceUUID::NamespaceForProjectDir(fs::path(s_project.generic_string() + "/")),
              IO::ResourceUUID::NamespaceForProjectDir(s_project));
}

TEST_F(ResourceUUIDNamespaceTest, ADifferentFolderGivesADifferentNamespace)
{
    EXPECT_NE(IO::ResourceUUID::NamespaceForProjectDir(s_root),
              IO::ResourceUUID::NamespaceForProjectDir(s_project));
}

#ifdef _WIN32
TEST_F(ResourceUUIDNamespaceTest, TheDriveLetterIsUpperCase)
{
    std::string lower = s_project.generic_string();
    ASSERT_GE(lower.size(), 2u);
    ASSERT_EQ(lower[1], ':');
    lower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));

    const std::string normalized = IO::ResourceUUID::NormalizeProjectDir(fs::path(lower));
    EXPECT_TRUE(std::isupper(static_cast<unsigned char>(normalized[0]))) << normalized;
    EXPECT_EQ(IO::ResourceUUID::NamespaceForProjectDir(fs::path(lower)),
              IO::ResourceUUID::NamespaceForProjectDir(s_project));
}
#endif
