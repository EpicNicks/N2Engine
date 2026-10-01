#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "text/FontAtlas.hpp"
#include "text/FontBackend.hpp"

namespace N2Engine::Text
{
    /// A loaded font face plus its SDF atlas: everything layout needs. CPU-only, so it works headless.
    class SdfFont
    {
    public:
        /// Called the first time layout meets a codepoint this font can't draw, once per font (later
        /// misses are still listed in TextLayout::missingCodepoints, but don't call it again)
        using MissingGlyphHandler = std::function<void(const SdfFont &font, char32_t codepoint)>;

        /// nullptr, with the reason in *error, if the backend can't read the data or the atlas can't be built
        [[nodiscard]] static std::unique_ptr<SdfFont> Create(const IFontBackend &backend, std::span<const std::byte> data,
                                                             const AtlasSettings &settings = {},
                                                             std::string *error = nullptr);
        [[nodiscard]] static std::unique_ptr<SdfFont> Create(std::unique_ptr<IFontFace> face,
                                                             const AtlasSettings &settings = {},
                                                             std::string *error = nullptr);

        SdfFont(const SdfFont &) = delete;
        SdfFont &operator=(const SdfFont &) = delete;

        [[nodiscard]] const IFontFace &GetFace() const { return *_face; }
        [[nodiscard]] const FontAtlas &GetAtlas() const { return _atlas; }
        [[nodiscard]] const FontMetrics &GetMetrics() const { return _metrics; }

        void SetMissingGlyphHandler(MissingGlyphHandler handler) { _missingGlyphHandler = std::move(handler); }
        /// Calls the handler unless a missing glyph was already reported for this font
        void ReportMissingGlyph(char32_t codepoint) const;
        [[nodiscard]] bool HasReportedMissingGlyph() const { return _missingGlyphReported.load(); }

    private:
        SdfFont(std::unique_ptr<IFontFace> face, FontAtlas atlas);

        std::unique_ptr<IFontFace> _face;
        FontAtlas _atlas;
        FontMetrics _metrics;
        MissingGlyphHandler _missingGlyphHandler;
        mutable std::atomic<bool> _missingGlyphReported{false};
    };
}
