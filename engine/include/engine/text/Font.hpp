#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <text/FontAtlas.hpp>
#include <text/SdfFont.hpp>
#include <text/TextLayout.hpp>

#include "engine/base/Asset.hpp"

namespace N2Engine::Text
{
    /// A font asset (.ttf/.otf): the font face plus its SDF atlas, built when it loads. It holds CPU data
    /// only, so it loads headless; renderers create their own texture from GetSdfFont().GetAtlas().
    ///
    /// Project fonts take their atlas settings from their .meta file's customData:
    ///   {"font": {"basePx": 48, "spreadPx": 8, "paddingPx": 2, "charset": "latin1", "extraChars": "..."}}
    /// Every key is optional; charset is "ascii" or "latin1", and extraChars is UTF-8 text whose
    /// characters are added to it.
    class Font final : public Base::Asset
    {
    public:
        Font() = default;

        /// nullptr (and an error logged) if the data isn't a readable font or the atlas can't be built
        [[nodiscard]] static std::shared_ptr<Font> CreateFromMemory(std::span<const std::byte> data,
                                                                   const AtlasSettings &settings = {},
                                                                   std::string_view debugName = "font");

        /// The built-in font (Noto Sans Regular, Latin subset) with the default atlas settings. Built on
        /// first use and shared afterwards. The embedded font is part of the build, so this is only null if
        /// the build itself is broken: that logs an error (and asserts in Debug), and every later call
        /// returns null too.
        [[nodiscard]] static std::shared_ptr<Font> GetDefault();

        /// The atlas settings in a .meta customData object (its "font" member), on top of defaults.
        /// Missing keys keep the default; a key with the wrong type or an out-of-range value is ignored
        /// with a warning.
        [[nodiscard]] static AtlasSettings ParseAtlasSettings(const nlohmann::json &customData,
                                                             const AtlasSettings &defaults = {});

        /// Registers the .ttf/.otf loader with Resources (and so ResourceLoader). Font.cpp already does
        /// this at static initialisation; calling it again is harmless, and guarantees the registration
        /// is linked in when nothing else in the program references Font.
        static void RegisterLoader();

        /// Loads a font file, with atlas settings from its .meta when ResourceLoader tracks it
        bool Load(const std::filesystem::path &path) override;
        /// What GetResourceType() returns, for code that needs it without an instance (asset metadata, a field's asset type)
        static constexpr std::string_view ResourceTypeName = "Font";
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        [[nodiscard]] bool IsLoaded() const { return _font != nullptr; }
        /// Requires IsLoaded(): there is no font to return otherwise
        [[nodiscard]] const SdfFont &GetSdfFont() const { return *_font; }
        /// The settings the atlas was built with, or the defaults if the font isn't loaded
        [[nodiscard]] const AtlasSettings &GetAtlasSettings() const;

        /// LayoutText with this font; an empty layout if the font isn't loaded
        [[nodiscard]] TextLayout Layout(std::string_view utf8, const LayoutOptions &options = {}) const;

    private:
        bool Build(std::span<const std::byte> data, const AtlasSettings &settings, std::string_view debugName);

        std::unique_ptr<SdfFont> _font;
    };

    /// The loader registered for .ttf and .otf
    std::shared_ptr<Base::Asset> LoadFontFromFile(const std::filesystem::path &path);
}
