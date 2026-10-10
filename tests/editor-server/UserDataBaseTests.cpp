#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <editor-server/EditorTls.hpp>
#include <engine/io/ProjectFile.hpp>

using namespace N2Engine;
namespace fs = std::filesystem;

namespace
{
    std::optional<std::string> ReadVariable(const char *name)
    {
#ifdef _WIN32
        char *buffer = nullptr;
        size_t length = 0;
        std::optional<std::string> value;
        if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr)
            value = buffer;
        std::free(buffer);
        return value;
#else
        const char *value = std::getenv(name);
        return value ? std::optional<std::string>(value) : std::nullopt;
#endif
    }

    void WriteVariable(const char *name, const std::optional<std::string> &value)
    {
#ifdef _WIN32
        (void)_putenv_s(name, value ? value->c_str() : ""); // an empty value removes the variable
#else
        if (value)
            (void)setenv(name, value->c_str(), 1);
        else
            (void)unsetenv(name);
#endif
    }

    /// Sets (or unsets) a variable for one test and puts the old value back after
    class ScopedVariable
    {
    public:
        ScopedVariable(const char *name, const std::optional<std::string> &value)
            : _name(name), _old(ReadVariable(name))
        {
            WriteVariable(name, value);
        }

        ~ScopedVariable() { WriteVariable(_name, _old); }
        ScopedVariable(const ScopedVariable &) = delete;
        ScopedVariable& operator=(const ScopedVariable &) = delete;

    private:
        const char *_name;
        std::optional<std::string> _old;
    };

    /// Where a per-user variable points on this OS, for the platform-neutral cases below
    constexpr const char *HomeVariable =
#ifdef _WIN32
        "APPDATA";
#else
        "HOME";
#endif
}

TEST(UserDataBaseTest, WithoutAHomeItIsTheWorkingDirectoryMarkerAndTheTlsFolderIsRefused)
{
#ifndef _WIN32
    const ScopedVariable xdg("XDG_DATA_HOME", std::nullopt);
#endif
    const ScopedVariable home(HomeVariable, std::nullopt);

    EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("."));
    // A key must never land in the working directory, which could be a project
    EXPECT_TRUE(Editor::EditorTlsServer::DefaultDirectory().empty());
    const auto loaded = Editor::EditorTlsServer::Load(Editor::EditorTlsServer::DefaultDirectory());
    EXPECT_FALSE(loaded.has_value());
}

#ifdef _WIN32
TEST(UserDataBaseTest, AppDataIsTheBase)
{
    const ScopedVariable home("APPDATA", std::string("C:\n2-test-appdata"));
    EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("C:\n2-test-appdata") / "N2Engine");
}
#else
TEST(UserDataBaseTest, XdgDataHomeWinsOverHome)
{
    const ScopedVariable xdg("XDG_DATA_HOME", std::string("/tmp/n2-xdg"));
    const ScopedVariable home("HOME", std::string("/tmp/n2-home"));
    EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("/tmp/n2-xdg") / "n2engine");
}

TEST(UserDataBaseTest, HomeAloneGivesTheXdgDefault)
{
    const ScopedVariable xdg("XDG_DATA_HOME", std::nullopt);
    const ScopedVariable home("HOME", std::string("/tmp/n2-home"));
    EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("/tmp/n2-home") / ".local" / "share" / "n2engine");
}

TEST(UserDataBaseTest, EmptyAndRelativeValuesCountAsUnset)
{
    const ScopedVariable home("HOME", std::string("/tmp/n2-home"));
    {
        const ScopedVariable xdg("XDG_DATA_HOME", std::string());
        EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("/tmp/n2-home") / ".local" / "share" / "n2engine");
    }
    {
        const ScopedVariable xdg("XDG_DATA_HOME", std::string("relative/data"));
        EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("/tmp/n2-home") / ".local" / "share" / "n2engine");
    }
    {
        const ScopedVariable xdg("XDG_DATA_HOME", std::nullopt);
        const ScopedVariable relativeHome("HOME", std::string("home"));
        EXPECT_EQ(IO::ProjectFile::UserDataBase(), fs::path("."));
    }
}
#endif
