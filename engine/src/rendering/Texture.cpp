#include "engine/rendering/Texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "engine/io/Vfs.hpp"

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
            // Through the virtual filesystem (a folder or a pack). The decoder takes at most 2 GiB; anything larger is
            // refused before it's read
            auto bytes = IO::Vfs::Instance().ReadBytesCapped(path, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
            if (!bytes)
            {
                return std::nullopt;
            }
            return std::move(*bytes);
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

        // ===== MakeOutline =====

        std::uint8_t ColourByte(const float value)
        {
            const float clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
            return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
        }

        /// floor(a / b) for b > 0 (C++ division truncates towards zero)
        std::int64_t FloorDiv(const std::int64_t a, const std::int64_t b)
        {
            const std::int64_t quotient = a / b;
            return (a % b != 0 && a < 0) ? quotient - 1 : quotient;
        }

        /**
         * Which pixels of a `width` x `height` grid lie within `radius` (Euclidean) of a pixel of `solid`: the exact squared
         * distance transform of Meijster, Roerdink and Hesselink (2000), two linear passes, so the cost does not grow with
         * the radius. `solid` and the result are row-major, one byte per pixel (0 or 1).
         */
        std::vector<std::uint8_t> DilateMask(const std::vector<std::uint8_t> &solid, const std::size_t width,
                                             const std::size_t height, const std::int64_t radius)
        {
            const auto m = static_cast<std::int64_t>(width);
            const auto n = static_cast<std::int64_t>(height);
            // Further than any real distance and than the radius (so a column with no solid pixel never counts as within
            // it), and still squarable in 64 bits
            const std::int64_t infinity = m + n + radius + 1;

            // Phase 1: g(x, y) = the distance along column x to the nearest solid pixel (infinity for none)
            std::vector<std::int64_t> g(width * height, infinity);
            const auto at = [width](const std::int64_t x, const std::int64_t y)
            { return static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x); };
            for (std::int64_t x = 0; x < m; ++x)
            {
                g[at(x, 0)] = solid[at(x, 0)] ? 0 : infinity;
                for (std::int64_t y = 1; y < n; ++y)
                {
                    g[at(x, y)] = solid[at(x, y)] ? 0 : std::min(infinity, g[at(x, y - 1)] + 1);
                }
                for (std::int64_t y = n - 2; y >= 0; --y)
                {
                    if (g[at(x, y + 1)] < g[at(x, y)])
                    {
                        g[at(x, y)] = g[at(x, y + 1)] + 1;
                    }
                }
            }

            // Phase 2, per row: the lower envelope of the parabolas (x - i)^2 + g(i)^2
            std::vector<std::uint8_t> result(width * height, 0);
            // s: the column of each parabola on the envelope; t: the first column where it is the lowest
            std::vector<std::int64_t> sColumns(width);
            std::vector<std::int64_t> tColumns(width);
            const auto s = [&](const std::int64_t q) -> std::int64_t & { return sColumns[static_cast<std::size_t>(q)]; };
            const auto t = [&](const std::int64_t q) -> std::int64_t & { return tColumns[static_cast<std::size_t>(q)]; };
            const std::int64_t radiusSquared = radius * radius;
            for (std::int64_t y = 0; y < n; ++y)
            {
                const auto gi = [&](const std::int64_t i) { return g[at(i, y)]; };
                const auto f = [&](const std::int64_t x, const std::int64_t i) { return (x - i) * (x - i) + gi(i) * gi(i); };
                const auto sep = [&](const std::int64_t i, const std::int64_t u)
                { return FloorDiv(u * u - i * i + gi(u) * gi(u) - gi(i) * gi(i), 2 * (u - i)); };

                std::int64_t q = 0;
                s(0) = 0;
                t(0) = 0;
                for (std::int64_t u = 1; u < m; ++u)
                {
                    while (q >= 0 && f(t(q), s(q)) > f(t(q), u))
                    {
                        --q;
                    }
                    if (q < 0)
                    {
                        q = 0;
                        s(0) = u;
                    }
                    else
                    {
                        const std::int64_t w = 1 + sep(s(q), u);
                        if (w < m)
                        {
                            ++q;
                            s(q) = u;
                            t(q) = w;
                        }
                    }
                }
                for (std::int64_t u = m - 1; u >= 0; --u)
                {
                    if (f(u, s(q)) <= radiusSquared)
                    {
                        result[at(u, y)] = 1;
                    }
                    if (u == t(q))
                    {
                        --q;
                    }
                }
            }
            return result;
        }

        struct OutlineCacheEntry
        {
            std::weak_ptr<const Texture> source;
            int width = 0;
            std::uint32_t colour = 0; // RGBA, 8 bits each
            std::shared_ptr<Texture> outline;
            std::uint64_t lastUse = 0;
        };

        struct OutlineCache
        {
            std::mutex mutex;
            std::vector<OutlineCacheEntry> entries;
            std::uint64_t clock = 0;
        };

        OutlineCache &GetOutlineCache()
        {
            // Leaked on purpose, as GpuCache is: a texture can be released after function-local statics are destroyed
            static auto *cache = new OutlineCache();
            return *cache;
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

    std::shared_ptr<Texture> Texture::MakeOutline(const std::shared_ptr<const Texture> &source, const int widthPixels,
                                                  const Common::Color &colour)
    {
        if (!source || !source->IsLoaded() || source->_width == 0 || source->_height == 0 ||
            source->_pixels.size() != static_cast<std::size_t>(source->_width) * source->_height * 4u)
        {
            Logger::Error("Texture::MakeOutline: the source texture is null or has no pixels");
            return nullptr;
        }
        if (widthPixels < 0 || widthPixels > MaxOutlineWidth)
        {
            Logger::Error(std::format("Texture::MakeOutline: width {} pixels is out of range (0 to {})", widthPixels,
                                      MaxOutlineWidth));
            return nullptr;
        }
        const std::uint64_t padding = static_cast<std::uint64_t>(widthPixels);
        const std::uint64_t outWidth = source->_width + 2 * padding;
        const std::uint64_t outHeight = source->_height + 2 * padding;
        if (outWidth > AssetImport::kMaxImageDimension || outHeight > AssetImport::kMaxImageDimension ||
            outWidth * outHeight * 4u > AssetImport::kMaxDecodedImageBytes)
        {
            Logger::Error(std::format("Texture::MakeOutline: a {} x {} outline is over the image size caps ({} a side, {} bytes)",
                                      outWidth, outHeight, AssetImport::kMaxImageDimension,
                                      AssetImport::kMaxDecodedImageBytes));
            return nullptr;
        }

        const std::array<std::uint8_t, 4> rgba{ColourByte(colour.r), ColourByte(colour.g), ColourByte(colour.b),
                                               ColourByte(colour.a)};
        const std::uint32_t colourKey = (std::uint32_t{rgba[0]} << 24) | (std::uint32_t{rgba[1]} << 16) |
                                        (std::uint32_t{rgba[2]} << 8) | std::uint32_t{rgba[3]};

        OutlineCache &cache = GetOutlineCache();
        const std::scoped_lock lock(cache.mutex);
        ++cache.clock;
        // A freed source's outlines can never be asked for again, and its address may be reused by a new texture
        std::erase_if(cache.entries, [](const OutlineCacheEntry &entry) { return entry.source.expired(); });
        for (OutlineCacheEntry &entry : cache.entries)
        {
            if (entry.width == widthPixels && entry.colour == colourKey && entry.source.lock() == source)
            {
                entry.lastUse = cache.clock;
                return entry.outline;
            }
        }

        const std::size_t sourceWidth = source->_width;
        const std::size_t sourceHeight = source->_height;
        const auto paddedWidth = static_cast<std::size_t>(outWidth);
        const auto paddedHeight = static_cast<std::size_t>(outHeight);
        const auto pad = static_cast<std::size_t>(padding);

        std::vector<std::uint8_t> solid(paddedWidth * paddedHeight, 0);
        bool anySolid = false;
        for (std::size_t y = 0; y < sourceHeight; ++y)
        {
            for (std::size_t x = 0; x < sourceWidth; ++x)
            {
                if (source->_pixels[(y * sourceWidth + x) * 4 + 3] >= OutlineSolidAlpha)
                {
                    solid[(y + pad) * paddedWidth + (x + pad)] = 1;
                    anySolid = true;
                }
            }
        }

        std::vector<std::uint8_t> pixels(paddedWidth * paddedHeight * 4, 0);
        if (anySolid)
        {
            const std::vector<std::uint8_t> inside = DilateMask(solid, paddedWidth, paddedHeight, widthPixels);
            for (std::size_t i = 0; i < inside.size(); ++i)
            {
                if (inside[i])
                {
                    std::copy(rgba.begin(), rgba.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i * 4));
                }
            }
        }

        auto outline = std::make_shared<Texture>();
        outline->_width = static_cast<std::uint32_t>(paddedWidth);
        outline->_height = static_cast<std::uint32_t>(paddedHeight);
        outline->_pixels = std::move(pixels);
        outline->_settings = source->_settings;
        // The padding is transparent: clamping keeps the far edge's texels from being filtered into this one
        outline->_settings.wrap = Renderer::Common::TextureWrap::ClampToEdge;

        if (cache.entries.size() >= OutlineCacheCapacity)
        {
            const auto oldest = std::ranges::min_element(cache.entries, {}, &OutlineCacheEntry::lastUse);
            cache.entries.erase(oldest);
        }
        cache.entries.push_back(OutlineCacheEntry{source, widthPixels, colourKey, outline, cache.clock});
        return outline;
    }

    std::size_t Texture::GetOutlineCacheSize()
    {
        OutlineCache &cache = GetOutlineCache();
        const std::scoped_lock lock(cache.mutex);
        std::erase_if(cache.entries, [](const OutlineCacheEntry &entry) { return entry.source.expired(); });
        return cache.entries.size();
    }

    void Texture::ClearOutlineCache()
    {
        OutlineCache &cache = GetOutlineCache();
        std::vector<OutlineCacheEntry> dropped;
        {
            const std::scoped_lock lock(cache.mutex);
            dropped.swap(cache.entries);
        }
        // The outlines are released here, outside the lock
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
