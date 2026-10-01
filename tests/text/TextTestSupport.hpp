#pragma once

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <text/DefaultFont.hpp>
#include <text/FontBackend.hpp>
#include <text/SdfFont.hpp>

// Every test that needs a font runs once per font backend in the build (stb_truetype now; FreeType too
// when P1b builds it), so all backends are held to the same behaviour.
namespace TextTestSupport
{
    using namespace N2Engine::Text;

    inline std::vector<FontBackendKind> Backends()
    {
        const auto available = GetAvailableFontBackends();
        return std::vector<FontBackendKind>(available.begin(), available.end());
    }

    inline std::string BackendName(const ::testing::TestParamInfo<FontBackendKind> &info)
    {
        return std::string(ToString(info.param));
    }

    /// The default font at the default atlas settings, built once per backend for the whole test run
    /// (rasterising the atlas is the slow part, especially in Debug)
    inline const SdfFont &SharedDefaultFont(const FontBackendKind kind)
    {
        static std::map<FontBackendKind, std::unique_ptr<SdfFont>> fonts;
        auto &font = fonts[kind];
        if (!font)
        {
            const auto backend = CreateFontBackend(kind);
            std::string error;
            font = backend ? SdfFont::Create(*backend, GetDefaultFontData(), AtlasSettings{}, &error) : nullptr;
            if (!font)
            {
                ADD_FAILURE() << "default font failed to load with " << ToString(kind) << ": " << error;
                std::abort(); // every caller dereferences the result
            }
        }
        return *font;
    }

    /// Small, quick atlas settings (ASCII at 16 px) for tests that build their own fonts
    inline AtlasSettings SmallSettings()
    {
        AtlasSettings settings;
        settings.basePx = 16.0f;
        settings.spreadPx = 4;
        settings.charset = Charset::Ascii;
        return settings;
    }

    /// A fresh font, for tests that change or compare a font's state; nullptr if it fails to load
    inline std::unique_ptr<SdfFont> MakeFont(const FontBackendKind kind, const AtlasSettings &settings = SmallSettings())
    {
        const auto backend = CreateFontBackend(kind);
        return backend ? SdfFont::Create(*backend, GetDefaultFontData(), settings) : nullptr;
    }

    class TextBackendTest : public ::testing::TestWithParam<FontBackendKind>
    {
    protected:
        [[nodiscard]] const SdfFont &DefaultFont() const { return SharedDefaultFont(GetParam()); }
    };
}
