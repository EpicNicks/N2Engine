// Checks on the fetched FreeType itself, built only with N2ENGINE_TEXT_FREETYPE: the build is the version
// the CMake pins, links into a test with the repo's static runtime, and contains the SDF renderer the
// FreeType font backend needs.
#ifdef N2ENGINE_TEXT_FREETYPE

#include <gtest/gtest.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

namespace
{
    class FreeTypeLibrary
    {
    public:
        FreeTypeLibrary() { _error = FT_Init_FreeType(&_library); }
        ~FreeTypeLibrary()
        {
            if (_error == 0)
            {
                FT_Done_FreeType(_library);
            }
        }
        FreeTypeLibrary(const FreeTypeLibrary &) = delete;
        FreeTypeLibrary &operator=(const FreeTypeLibrary &) = delete;

        [[nodiscard]] FT_Error Error() const { return _error; }
        [[nodiscard]] FT_Library Get() const { return _library; }

    private:
        FT_Library _library = nullptr;
        FT_Error _error = 0;
    };
}

TEST(FreeTypeBuildTest, InitialisesAndIsThePinnedVersion)
{
    const FreeTypeLibrary library;
    ASSERT_EQ(library.Error(), 0);

    FT_Int major = 0;
    FT_Int minor = 0;
    FT_Int patch = 0;
    FT_Library_Version(library.Get(), &major, &minor, &patch);
    EXPECT_EQ(major, 2);
    EXPECT_EQ(minor, 13);
    EXPECT_EQ(patch, 3);
}

TEST(FreeTypeBuildTest, HasTheSdfRendererAndItsSpreadProperty)
{
    const FreeTypeLibrary library;
    ASSERT_EQ(library.Error(), 0);
    EXPECT_NE(FT_Get_Module(library.Get(), "sdf"), nullptr);

    FT_Int spread = 6;
    EXPECT_EQ(FT_Property_Set(library.Get(), "sdf", "spread", &spread), 0);
    FT_Int readBack = 0;
    EXPECT_EQ(FT_Property_Get(library.Get(), "sdf", "spread", &readBack), 0);
    EXPECT_EQ(readBack, 6);
}

#endif
