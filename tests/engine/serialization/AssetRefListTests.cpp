#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

#include "../rendering/MeshTestSupport.hpp"

// RegisterAssetRefList (a list of asset UUIDs, empty slots as null) and the built-in check asset references make,
// through a component that saves a list of textures and a mesh

using namespace N2Engine;
using Rendering::Mesh;
using Rendering::Texture;
using MeshTestSupport::WarningCapture;
using json = nlohmann::json;

namespace
{
    class AssetListHolder final : public SerializableComponent
    {
    public:
        explicit AssetListHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterAssetRefList("textures", textures);
            RegisterAssetRef("mesh", mesh);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "AssetRefListTest_Holder"; }

        std::vector<std::shared_ptr<Texture>> textures;
        std::shared_ptr<Mesh> mesh;
    };

    /// Textures registered at runtime (random UUIDs, found by this run), unregistered when it goes
    struct RegisteredTextures
    {
        std::vector<std::shared_ptr<Texture>> textures;

        explicit RegisteredTextures(const int count)
        {
            for (int i = 0; i < count; ++i)
            {
                auto texture = Texture::Create(1, 1, std::vector<std::uint8_t>{static_cast<std::uint8_t>(i), 0, 0, 255});
                IO::Resources::Instance().RegisterAsset(texture);
                textures.push_back(std::move(texture));
            }
        }
        ~RegisteredTextures()
        {
            for (const auto &texture : textures)
            {
                IO::Resources::Instance().UnregisterAsset(texture->GetUUID());
            }
        }
    };

    AssetListHolder *Load(GameObject &gameObject, const json &data)
    {
        auto *holder = gameObject.AddComponent<AssetListHolder>();
        holder->Deserialize(data, nullptr);
        return holder;
    }
}

TEST(AssetRefListTest, SavesUuidsInOrderWithNullForEmptySlots)
{
    const RegisteredTextures registered(2);
    const auto go = GameObject::Create("Saver");
    auto *holder = go->AddComponent<AssetListHolder>();
    holder->textures = {registered.textures[0], nullptr, registered.textures[1]};

    const json saved = holder->Serialize();
    ASSERT_TRUE(saved.at("textures").is_array());
    ASSERT_EQ(saved.at("textures").size(), 3u);
    EXPECT_EQ(saved["textures"][0], registered.textures[0]->GetUUID().ToString());
    EXPECT_TRUE(saved["textures"][1].is_null());
    EXPECT_EQ(saved["textures"][2], registered.textures[1]->GetUUID().ToString());

    holder->textures.clear();
    EXPECT_EQ(holder->Serialize().at("textures"), json::array()) << "an empty list saves as []";
}

TEST(AssetRefListTest, RoundTripsKeepingSlotsAndOrder)
{
    const RegisteredTextures registered(3);
    const auto go = GameObject::Create("RoundTrip");
    auto *holder = go->AddComponent<AssetListHolder>();
    holder->textures = {registered.textures[2], nullptr, registered.textures[0], registered.textures[2]};
    const json saved = holder->Serialize();

    WarningCapture capture;
    const auto loadedGo = GameObject::Create("Loaded");
    const auto *loaded = Load(*loadedGo, saved);
    ASSERT_EQ(loaded->textures.size(), 4u);
    EXPECT_EQ(loaded->textures[0], registered.textures[2]);
    EXPECT_EQ(loaded->textures[1], nullptr);
    EXPECT_EQ(loaded->textures[2], registered.textures[0]);
    EXPECT_EQ(loaded->textures[3], registered.textures[2]) << "the same asset may fill several slots";
    EXPECT_TRUE(capture.messages.empty());
}

TEST(AssetRefListTest, AMissingAssetLoadsAsAnEmptySlotWithAWarning)
{
    const RegisteredTextures registered(1);
    const json data = {{"textures", json::array({Math::UUID::Random().ToString(), registered.textures[0]->GetUUID().ToString(),
                                                 "not-a-uuid"})}};
    WarningCapture capture;
    const auto go = GameObject::Create("Missing");
    const auto *loaded = Load(*go, data);
    ASSERT_EQ(loaded->textures.size(), 3u) << "slots keep their positions";
    EXPECT_EQ(loaded->textures[0], nullptr);
    EXPECT_EQ(loaded->textures[1], registered.textures[0]);
    EXPECT_EQ(loaded->textures[2], nullptr);
    EXPECT_TRUE(capture.Mentions("not found"));
    EXPECT_TRUE(capture.Mentions("textures[0]")) << "names the slot";
}

TEST(AssetRefListTest, AMissingKeyOrANonArrayLoadsAnEmptyList)
{
    const RegisteredTextures registered(1);
    const auto go = GameObject::Create("Shapes");
    auto *holder = go->AddComponent<AssetListHolder>();
    holder->textures = {registered.textures[0]};
    holder->Deserialize(json::object(), nullptr);
    EXPECT_TRUE(holder->textures.empty()) << "a missing key";

    holder->textures = {registered.textures[0]};
    WarningCapture capture;
    holder->Deserialize(json{{"textures", "oops"}}, nullptr);
    EXPECT_TRUE(holder->textures.empty());
    EXPECT_TRUE(capture.Mentions("isn't an array"));

    holder->textures = {registered.textures[0]};
    holder->Deserialize(json{{"textures", nullptr}}, nullptr);
    EXPECT_TRUE(holder->textures.empty()) << "null";
}

TEST(AssetRefListTest, BuiltinMeshesResolveByTheirFixedUuids)
{
    // Not registered anywhere: the reference checks Mesh::FindBuiltin before Resources
    const auto go = GameObject::Create("Builtin");
    auto *holder = go->AddComponent<AssetListHolder>();
    holder->mesh = Mesh::GetBuiltin(Rendering::BuiltinMesh::Sphere);
    const json saved = holder->Serialize();
    EXPECT_EQ(saved.at("mesh"), "6e32656e-6d65-5348-0001-000000000002");

    WarningCapture capture;
    const auto loadedGo = GameObject::Create("BuiltinLoaded");
    const auto *loaded = Load(*loadedGo, saved);
    EXPECT_EQ(loaded->mesh, Mesh::GetBuiltin(Rendering::BuiltinMesh::Sphere));
    EXPECT_TRUE(capture.messages.empty());
}
