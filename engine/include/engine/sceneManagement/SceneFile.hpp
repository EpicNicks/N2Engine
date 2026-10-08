#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/base/Asset.hpp"

namespace N2Engine
{
    /**
     * A .scene file loaded as an asset: the scene's JSON, as Scene::Serialize wrote it. It is what ResourceLoader
     * loads for a .scene path or UUID, so scenes are assets with path-derived UUIDs like any other (a project file's
     * startupScene names one). It is data only: building the live Scene from it is Scene::FromJSON's job, and a
     * Scene opened from a file takes this file's UUID (Asset::SetUUID), so a scene's UUID is stable across runs.
     * Its resource type is "Scene", the same as Scene's own: there is one kind of scene asset, in two forms (the
     * file's data, cached by ResourceLoader as a shared_ptr, and the live Scene, owned by SceneManager).
     */
    class SceneFile : public Base::Asset
    {
    public:
        static constexpr std::string_view ResourceTypeName = "Scene";
        static constexpr std::string_view Extension = ".scene";

        /// Reads and parses the file; false (logged) when it can't be read or isn't a JSON object
        bool Load(const std::filesystem::path &path) override;
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        /// The scene's JSON (an object)
        [[nodiscard]] const nlohmann::json &GetData() const { return _data; }
        /// The name the scene's JSON gives it, or "" when it has none
        [[nodiscard]] std::string GetSceneName() const;

        /// Registers the .scene loader (idempotent). ResourceLoader::Initialize calls it, so every project's scene
        /// files are scanned and get .meta UUIDs.
        static void RegisterLoader();

    private:
        nlohmann::json _data = nlohmann::json::object();
    };
}
