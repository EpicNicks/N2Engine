#include "text/FontBackend.hpp"

#include <algorithm>
#include <array>

#include "StbTrueTypeBackend.hpp"

namespace N2Engine::Text
{
    namespace
    {
        // Every backend this build contains, default first. The FreeType backend (P1b) adds itself here
        // under its CMake option; tests iterate this list, so they run against each one.
        constexpr std::array kAvailableBackends{
            FontBackendKind::StbTrueType,
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
            return nullptr; // not built yet
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
