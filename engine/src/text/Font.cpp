#include "engine/text/Font.hpp"

#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <text/DefaultFont.hpp>
#include <text/FontBackend.hpp>
#include <text/Utf8.hpp>

#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/Resources.hpp"

namespace N2Engine::Text
{
    namespace
    {
        // Lives here because anything that uses Font links this file. RegisterLoader is public too, for
        // programs that only ever load fonts by path.
        struct FontLoaderRegistrar
        {
            FontLoaderRegistrar()
            {
                Font::RegisterLoader();
            }
        } g_fontLoaderRegistrar;

        std::optional<std::vector<std::byte>> ReadFontFile(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                return std::nullopt;
            }
            const std::streamsize size = file.tellg();
            if (size < 0)
            {
                return std::nullopt;
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), size))
            {
                return std::nullopt;
            }
            return bytes;
        }

        // A project font's settings come from its .meta; anything else (or a font outside the project) uses
        // the defaults
        AtlasSettings SettingsFor(const std::filesystem::path &path)
        {
            const IO::ResourceLoader &loader = IO::ResourceLoader::Instance();
            if (loader.GetAssetsRoot().empty())
            {
                return {};
            }
            try
            {
                if (const IO::AssetMetadata *meta = loader.GetMetadata(loader.MakeResourcePath(path)))
                {
                    return Font::ParseAtlasSettings(meta->customData);
                }
            }
            catch (const std::exception &)
            {
                // Not a path ResourceLoader can express, so not one of its assets
            }
            return {};
        }

        void WarnBadSetting(const std::string_view key, const nlohmann::json &value, const std::string_view expected)
        {
            Logger::Warn(std::format("Font settings: ignoring \"{}\": {} (expected {})", key, value.dump(), expected));
        }
    }

    AtlasSettings Font::ParseAtlasSettings(const nlohmann::json &customData, const AtlasSettings &defaults)
    {
        AtlasSettings settings = defaults;
        if (!customData.is_object())
        {
            return settings;
        }
        const auto fontIt = customData.find("font");
        if (fontIt == customData.end())
        {
            return settings;
        }
        if (!fontIt->is_object())
        {
            WarnBadSetting("font", *fontIt, "an object");
            return settings;
        }
        const nlohmann::json &font = *fontIt;

        if (const auto it = font.find("basePx"); it != font.end())
        {
            if (it->is_number() && it->get<double>() >= 4.0 && it->get<double>() <= 512.0)
                settings.basePx = it->get<float>();
            else
                WarnBadSetting("basePx", *it, "a number from 4 to 512");
        }
        if (const auto it = font.find("spreadPx"); it != font.end())
        {
            if (it->is_number_integer() && it->get<long long>() >= 1 && it->get<long long>() <= 64)
                settings.spreadPx = it->get<int>();
            else
                WarnBadSetting("spreadPx", *it, "an integer from 1 to 64");
        }
        if (const auto it = font.find("paddingPx"); it != font.end())
        {
            if (it->is_number_integer() && it->get<long long>() >= 0 && it->get<long long>() <= 64)
                settings.paddingPx = it->get<int>();
            else
                WarnBadSetting("paddingPx", *it, "an integer from 0 to 64");
        }
        if (const auto it = font.find("charset"); it != font.end())
        {
            if (it->is_string() && it->get<std::string>() == "ascii")
                settings.charset = Charset::Ascii;
            else if (it->is_string() && it->get<std::string>() == "latin1")
                settings.charset = Charset::Latin1;
            else
                WarnBadSetting("charset", *it, "\"ascii\" or \"latin1\"");
        }
        if (const auto it = font.find("extraChars"); it != font.end())
        {
            if (it->is_string())
                settings.extraChars = DecodeUtf8(it->get<std::string>());
            else
                WarnBadSetting("extraChars", *it, "a string");
        }
        return settings;
    }

    void Font::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            for (const char *extension : {".ttf", ".otf"})
            {
                IO::Resources::Instance().RegisterLoader(extension, LoadFontFromFile);
            }
        });
    }

    std::shared_ptr<Font> Font::CreateFromMemory(const std::span<const std::byte> data, const AtlasSettings &settings,
                                                 const std::string_view debugName)
    {
        auto font = std::make_shared<Font>();
        if (!font->Build(data, settings, debugName))
        {
            return nullptr;
        }
        return font;
    }

    std::shared_ptr<Font> Font::GetDefault()
    {
        // Built once; the atlas takes a moment to rasterise and every caller can share it
        static const std::shared_ptr<Font> defaultFont =
            CreateFromMemory(GetDefaultFontData(), AtlasSettings{}, "default font (Noto Sans Regular)");
        return defaultFont;
    }

    bool Font::Load(const std::filesystem::path &path)
    {
        const auto data = ReadFontFile(path);
        if (!data)
        {
            Logger::Error(std::format("Cannot read font file {}", path.string()));
            return false;
        }
        return Build(*data, SettingsFor(path), path.string());
    }

    bool Font::Build(const std::span<const std::byte> data, const AtlasSettings &settings, const std::string_view debugName)
    {
        const auto backend = CreateDefaultFontBackend();
        if (!backend)
        {
            Logger::Error("No font backend is available");
            return false;
        }

        std::string error;
        auto font = SdfFont::Create(*backend, data, settings, &error);
        if (!font)
        {
            Logger::Error(std::format("Cannot load font {}: {}", debugName, error));
            return false;
        }

        font->SetMissingGlyphHandler([name = std::string(debugName)](const SdfFont &, const char32_t codepoint)
        {
            Logger::Warn(std::format("Font {} has no glyph for U+{:04X}; it draws as the fallback glyph "
                                     "(reported once per font)", name, static_cast<std::uint32_t>(codepoint)));
        });
        _font = std::move(font);
        return true;
    }

    TextLayout Font::Layout(const std::string_view utf8, const LayoutOptions &options) const
    {
        if (!_font)
        {
            return {};
        }
        return LayoutText(*_font, utf8, options);
    }

    std::shared_ptr<Base::Asset> LoadFontFromFile(const std::filesystem::path &path)
    {
        auto font = std::make_shared<Font>();
        if (!font->Load(path))
        {
            return nullptr;
        }
        return font;
    }
}
