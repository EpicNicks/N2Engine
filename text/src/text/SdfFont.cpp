#include "text/SdfFont.hpp"

#include <utility>

namespace N2Engine::Text
{
    SdfFont::SdfFont(std::unique_ptr<IFontFace> face, FontAtlas atlas)
        : _face(std::move(face)), _atlas(std::move(atlas)), _metrics(_face->GetMetrics())
    {
    }

    std::unique_ptr<SdfFont> SdfFont::Create(const IFontBackend &backend, const std::span<const std::byte> data,
                                             const AtlasSettings &settings, std::string *error)
    {
        auto face = backend.LoadFace(data, error);
        if (!face)
        {
            return nullptr;
        }
        return Create(std::move(face), settings, error);
    }

    std::unique_ptr<SdfFont> SdfFont::Create(std::unique_ptr<IFontFace> face, const AtlasSettings &settings,
                                             std::string *error)
    {
        if (!face)
        {
            if (error != nullptr)
            {
                *error = "no font face";
            }
            return nullptr;
        }

        auto atlas = FontAtlas::Build(*face, settings, error);
        if (!atlas)
        {
            return nullptr;
        }
        // The constructor is private, so make_unique can't reach it
        return std::unique_ptr<SdfFont>(new SdfFont(std::move(face), std::move(*atlas)));
    }

    void SdfFont::ReportMissingGlyph(const char32_t codepoint) const
    {
        if (!_missingGlyphReported.exchange(true) && _missingGlyphHandler)
        {
            _missingGlyphHandler(*this, codepoint);
        }
    }
}
