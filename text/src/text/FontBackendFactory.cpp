#include "text/FontBackend.hpp"

#include <algorithm>
#include <array>

#include "StbTrueTypeBackend.hpp"
#ifdef N2ENGINE_TEXT_FREETYPE
#include "FreeTypeBackend.hpp"
#endif

namespace N2Engine::Text
{
    namespace
    {
        // Every backend this build contains, default first. FreeType is here when the build has the CMake
        // option N2ENGINE_TEXT_FREETYPE; tests iterate this list, so they run against each one.
        constexpr std::array kAvailableBackends{
            FontBackendKind::StbTrueType,
#ifdef N2ENGINE_TEXT_FREETYPE
            FontBackendKind::FreeType,
#endif
        };
    }

    std::string_view ToString(const FontBackendKind kind)
    {
        switch (kind)
        {
        case FontBackendKind::StbTrueType:
            return "StbTrueType";
        case FontBackendKind::FreeType:
            return "FreeType";
        }
        return "Unknown";
    }

    std::span<const FontBackendKind> GetAvailableFontBackends()
    {
        return kAvailableBackends;
    }

    bool IsFontBackendAvailable(const FontBackendKind kind)
    {
        return std::ranges::find(kAvailableBackends, kind) != kAvailableBackends.end();
    }

    std::unique_ptr<IFontBackend> CreateFontBackend(const FontBackendKind kind)
    {
        switch (kind)
        {
        case FontBackendKind::StbTrueType:
            return Detail::CreateStbTrueTypeBackend();
        case FontBackendKind::FreeType:
#ifdef N2ENGINE_TEXT_FREETYPE
            return Detail::CreateFreeTypeBackend();
#else
            return nullptr; // not in this build
#endif
        }
        return nullptr;
    }

    FontBackendKind GetDefaultFontBackendKind()
    {
        return kAvailableBackends.front();
    }

    std::unique_ptr<IFontBackend> CreateDefaultFontBackend()
    {
        return CreateFontBackend(GetDefaultFontBackendKind());
    }
}
