#include "engine/io/LatePacks.hpp"

#include <algorithm>
#include <exception>
#include <format>

#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"

namespace N2Engine::IO::LatePacks
{
    namespace
    {
        struct Folder
        {
            std::filesystem::path root;
            bool arrived = false;
            bool failed = false;
        };

        std::vector<Folder> &Folders()
        {
            static std::vector<Folder> folders;
            return folders;
        }

        std::filesystem::path Normal(const std::filesystem::path &path)
        {
            std::filesystem::path normal = path.lexically_normal();
            // "a/b/" and "a/b" are the same folder
            if (!normal.empty() && !normal.has_filename())
            {
                normal = normal.parent_path();
            }
            return normal;
        }

        /// Whether `path` is `root` or inside it, by whole path segments
        bool Inside(const std::filesystem::path &root, const std::filesystem::path &path)
        {
            auto r = root.begin();
            auto p = path.begin();
            for (; r != root.end(); ++r, ++p)
            {
                if (p == path.end() || *p != *r)
                {
                    return false;
                }
            }
            return true;
        }
    }

    void Register(const std::filesystem::path &folder)
    {
        const std::filesystem::path root = Normal(folder);
        auto &folders = Folders();
        if (std::ranges::any_of(folders, [&](const Folder &f) { return f.root == root; }))
        {
            return;
        }
        folders.push_back(Folder{root, false});
    }

    bool IsPending(const std::filesystem::path &path)
    {
        const std::filesystem::path normal = Normal(path);
        return std::ranges::any_of(Folders(), [&](const Folder &f) { return !f.arrived && Inside(f.root, normal); });
    }

    std::vector<std::filesystem::path> Pending()
    {
        std::vector<std::filesystem::path> out;
        for (const Folder &f : Folders())
        {
            if (!f.arrived)
            {
                out.push_back(f.root);
            }
        }
        return out;
    }

    std::vector<std::filesystem::path> Arrived()
    {
        std::vector<std::filesystem::path> out;
        for (const Folder &f : Folders())
        {
            if (f.arrived)
            {
                out.push_back(f.root);
            }
        }
        return out;
    }

    bool Arrive(const std::filesystem::path &folder, std::unique_ptr<IMount> mount)
    {
        const std::filesystem::path root = Normal(folder);
        auto &folders = Folders();
        const auto it = std::ranges::find_if(folders, [&](const Folder &f) { return f.root == root; });
        if (it == folders.end() || it->arrived || !mount)
        {
            return false;
        }
        Vfs::Instance().Mount(root, std::move(mount));
        it->arrived = true;
        it->failed = false;
        ResourceLoader &loader = ResourceLoader::Instance();
        if (!loader.GetAssetsRoot().empty())
        {
            // The rescan can throw (a folder that cannot be walked): this runs from the loader's call, which must get an
            // answer, so the error is logged; the pack stays mounted and the next rescan (an asset change) tries again
            try
            {
                const auto result = loader.RescanAssets();
                (void)result;
            }
            catch (const std::exception &error)
            {
                Logger::Error(std::format("Late pack {}: the asset rescan failed ({}); its files load after the next rescan",
                                          PathToUtf8(root), error.what()));
            }
        }
        Logger::Info(std::format("Late pack arrived: {} is available", PathToUtf8(root)));
        return true;
    }

    void Fail(const std::filesystem::path &folder)
    {
        const std::filesystem::path root = Normal(folder);
        for (Folder &f : Folders())
        {
            if (f.root == root && !f.arrived)
            {
                f.failed = true;
                Logger::Error(std::format("Late pack for {} could not be fetched: its files are not available", PathToUtf8(root)));
            }
        }
    }

    std::vector<std::filesystem::path> Failed()
    {
        std::vector<std::filesystem::path> out;
        for (const Folder &f : Folders())
        {
            if (f.failed && !f.arrived)
            {
                out.push_back(f.root);
            }
        }
        return out;
    }

    void Reset()
    {
        Folders().clear();
    }
}
