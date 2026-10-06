#include "engine/rendering/Texture.hpp"

#include <cstddef>
#include <exception>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include <assetimport/ImageDecoder.hpp>

#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/Resources.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        // Lives here because anything that uses Texture links this file. RegisterLoader is public too, and
        // ResourceLoader::Initialize calls it, for programs that never name Texture.
        struct TextureLoaderRegistrar
        {
            TextureLoaderRegistrar()
            {
                Texture::RegisterLoader();
            }
        } g_textureLoaderRegistrar;

        std::optional<std::vector<std::uint8_t>> ReadImageFile(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                return std::nullopt;
            }
            const std::streamsize size = file.tellg();
            // The decoder takes at most 2 GiB; anything larger is refused before it's read
            if (size < 0 || static_cast<unsigned long long>(size) > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), size))
            {
                return std::nullopt;
            }
            return bytes;
        }

        // A project texture's settings come from its .meta; anything else (or a file outside the project) uses
        // the defaults
        TextureSettings SettingsFor(const std::filesystem::path &path)
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
                    return Texture::ParseSettings(meta->customData);
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
            Logger::Warn(std::format("Texture settings: ignoring \"{}\": {} (expected {})", key, value.dump(), expected));
        }

        void ReadBool(const nlohmann::json &texture, const char *key, bool &out)
        {
            if (const auto it = texture.find(key); it != texture.end())
            {
                if (it->is_boolean())
                    out = it->get<bool>();
                else
                    WarnBadSetting(key, *it, "true or false");
            }
        }
    }

    Renderer::Common::TextureOptions TextureSettings::ToTextureOptions() const
    {
        Renderer::Common::TextureOptions options;
        options.filter = filter;
        options.wrap = wrap;
        options.mipmaps = mipmaps;
        return options;
    }

    TextureSettings Texture::ParseSettings(const nlohmann::json &customData, const TextureSettings &defaults)
    {
        TextureSettings settings = defaults;
        if (!customData.is_object())
        {
            return settings;
        }
        const auto textureIt = customData.find("texture");
        if (textureIt == customData.end())
        {
            return settings;
        }
        if (!textureIt->is_object())
        {
            WarnBadSetting("texture", *textureIt, "an object");
            return settings;
        }
        const nlohmann::json &texture = *textureIt;

        ReadBool(texture, "srgb", settings.srgb);
        if (const auto it = texture.find("filter"); it != texture.end())
        {
            if (it->is_string() && it->get<std::string>() == "linear")
                settings.filter = Renderer::Common::TextureFilter::Linear;
            else if (it->is_string() && it->get<std::string>() == "nearest")
                settings.filter = Renderer::Common::TextureFilter::Nearest;
            else
                WarnBadSetting("filter", *it, "\"linear\" or \"nearest\"");
        }
        if (const auto it = texture.find("wrap"); it != texture.end())
        {
            if (it->is_string() && it->get<std::string>() == "repeat")
                settings.wrap = Renderer::Common::TextureWrap::Repeat;
            else if (it->is_string() && it->get<std::string>() == "clamp")
                settings.wrap = Renderer::Common::TextureWrap::ClampToEdge;
            else
                WarnBadSetting("wrap", *it, "\"repeat\" or \"clamp\"");
        }
        ReadBool(texture, "mipmaps", settings.mipmaps);
        ReadBool(texture, "flipY", settings.flipY);
        return settings;
    }

    void Texture::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            for (const char *extension : {".png", ".jpg", ".jpeg", ".tga", ".bmp"})
            {
                IO::Resources::Instance().RegisterLoader(extension, LoadTextureFromFile);
            }
        });
    }

    std::shared_ptr<Texture> Texture::Create(const std::uint32_t width, const std::uint32_t height,
                                             const std::span<const std::uint8_t> rgbaPixels,
                                             const Renderer::Common::TextureOptions &options)
    {
        const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        if (width == 0 || height == 0 || rgbaPixels.size() != expected)
        {
            Logger::Error(std::format("Texture::Create: {} x {} RGBA needs {} bytes of pixels, got {}", width, height,
                                      expected, rgbaPixels.size()));
            return nullptr;
        }
        auto texture = std::make_shared<Texture>();
        texture->_width = width;
        texture->_height = height;
        texture->_pixels.assign(rgbaPixels.begin(), rgbaPixels.end());
        texture->_settings.filter = options.filter;
        texture->_settings.wrap = options.wrap;
        texture->_settings.mipmaps = options.mipmaps;
        return texture;
    }

    std::shared_ptr<Texture> Texture::CreateFromEncoded(const std::span<const std::uint8_t> fileBytes,
                                                        const TextureSettings &settings, const std::string_view debugName)
    {
        auto texture = std::make_shared<Texture>();
        if (!texture->Decode(fileBytes, settings, debugName))
        {
            return nullptr;
        }
        return texture;
    }

    bool Texture::Load(const std::filesystem::path &path)
    {
        const auto bytes = ReadImageFile(path);
        if (!bytes)
        {
            Logger::Error(std::format("Cannot read image file {}", path.string()));
            return false;
        }
        if (!Decode(*bytes, SettingsFor(path), path.string()))
        {
            return false;
        }
        _sourcePath = path.string();
        return true;
    }

    bool Texture::Decode(const std::span<const std::uint8_t> fileBytes, const TextureSettings &settings,
                         const std::string_view debugName)
    {
        AssetImport::ImageDecodeOptions options;
        options.flipY = settings.flipY;
        auto decoded = AssetImport::DecodeImage(fileBytes, options);
        if (!decoded)
        {
            Logger::Error(std::format("Cannot load texture {}: {}", debugName, decoded.error().message));
            return false;
        }
        _width = decoded->width;
        _height = decoded->height;
        _pixels = std::move(decoded->pixels);
        _settings = settings;
        return true;
    }

    std::shared_ptr<Base::Asset> LoadTextureFromFile(const std::filesystem::path &path)
    {
        auto texture = std::make_shared<Texture>();
        if (!texture->Load(path))
        {
            return nullptr;
        }
        return texture;
    }
}
