#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/base/Asset.hpp"

namespace N2Engine
{
    /**
     * A .prefab file loaded as an asset: the prefab's JSON, as Prefab::Serialize writes it ({"name", "rootObject"}).
     * ResourceLoader indexes it, so a prefab has a .meta UUID like any other asset and the editor's ListAssets shows it
     * with the resource type "Prefab". It is data only: PrefabManager::InstantiatePrefab(s) builds objects from it.
     * Loading checks the text's shape (nesting depth, a JSON object with a string "name" and an object "rootObject")
     * but builds no GameObject; whether the object data itself is well formed is found out, and refused, when it is
     * instantiated.
     */
    class PrefabFile : public Base::Asset
    {
    public:
        static constexpr std::string_view ResourceTypeName = "Prefab";
        static constexpr std::string_view Extension = ".prefab";

        /// Parses prefab file text, applying the same checks Load does. The nesting is checked on the text before
        /// anything is parsed, as for a scene file (a prefab file is untrusted data). On failure the message says what
        /// is wrong with the text and holds neither a path nor any of the text.
        static std::expected<nlohmann::json, std::string> Parse(std::string_view text);

        /// Reads and parses the file; false (logged) when it can't be read or isn't a prefab file
        bool Load(const std::filesystem::path &path) override;
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        /// The prefab's JSON (an object with "name" and "rootObject")
        [[nodiscard]] const nlohmann::json &GetData() const { return _data; }
        /// The prefab's name, or "" before a successful Load
        [[nodiscard]] std::string GetPrefabName() const;

        /// Registers the .prefab loader (idempotent). ResourceLoader::Initialize calls it, so every project's prefab
        /// files are scanned and get .meta UUIDs.
        static void RegisterLoader();

    private:
        nlohmann::json _data = nlohmann::json::object();
    };
}
