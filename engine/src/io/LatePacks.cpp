#include "engine/io/LatePacks.hpp"

#include <algorithm>
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
        ResourceLoader &loader = ResourceLoader::Instance();
        if (!loader.GetAssetsRoot().empty())
        {
            const auto result = loader.RescanAssets();
            (void)result;
        }
        Logger::Info(std::format("Late pack arrived: {} is available", PathToUtf8(root)));
        return true;
    }

    void Reset()
    {
        Folders().clear();
    }
}
