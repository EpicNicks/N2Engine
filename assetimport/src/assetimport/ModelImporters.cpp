#include "assetimport/ModelImporters.hpp"

#include <algorithm>
#include <cctype>

#include "assetimport/GltfImporter.hpp"
#ifdef N2ENGINE_MODEL_UFBX
#include "assetimport/UfbxImporter.hpp"
#endif

namespace N2Engine::AssetImport
{
    namespace
    {
        const std::vector<const IModelImporter *> &Importers()
        {
            static const GltfImporter gltf;
#ifdef N2ENGINE_MODEL_UFBX
            static const UfbxImporter ufbx;
            static const std::vector<const IModelImporter *> all = {&gltf, &ufbx};
#else
            static const std::vector<const IModelImporter *> all = {&gltf};
#endif
            return all;
        }

        std::string Lower(const std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }
    }

    std::span<const IModelImporter *const> GetModelImporters()
    {
        return Importers();
    }

    const IModelImporter *FindModelImporter(const std::string_view extension)
    {
        const std::string lower = Lower(extension);
        for (const IModelImporter *importer : Importers())
        {
            if (importer->HandlesExtension(lower))
            {
                return importer;
            }
        }
        return nullptr;
    }

    std::vector<std::string> GetModelExtensions()
    {
        std::vector<std::string> extensions = {".gltf", ".glb"};
#ifdef N2ENGINE_MODEL_UFBX
        extensions.emplace_back(".fbx");
        extensions.emplace_back(".obj");
#endif
        return extensions;
    }

    bool IsUfbxAvailable()
    {
#ifdef N2ENGINE_MODEL_UFBX
        return true;
#else
        return false;
#endif
    }
}
