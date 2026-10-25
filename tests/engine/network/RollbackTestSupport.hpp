#pragma once

// A small deterministic game for the rollback tests (netcode C5): avatars that move and shoot, shots that fly for a few
// ticks and hit (spawn, despawn, a game-owned random stream, replicated fields that are not quantisable, and effects
// that go through the rollback-aware event queue). The same Arena class is driven by a lockstep loop and by rollback
// sessions, which is what the equality tests compare.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <math/DeterministicRandom.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/RollbackEvents.hpp"
#include "engine/network/RollbackWorld.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/network/n2net/Lockstep.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Network::Testing::Rollback
{
    inline constexpr const char *kShotPrefab = "RbShotPrefab";
    inline constexpr std::uint32_t kEventFire = 1;
    inline constexpr std::uint32_t kEventHit = 2;

    class Avatar final : public SerializableComponent
    {
    public:
        explicit Avatar(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("x", x).Replicated();
            RegisterMember("y", y).Replicated();
            RegisterMember("score", score).Replicated();
            RegisterMember("lastFire", lastFire).Replicated();
            RegisterMember("presentationOnly", presentationOnly);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "RbAvatar"; }

        float x = 0.0f;
        float y = 0.0f;
        int score = 0;
        int lastFire = -100;
        int presentationOnly = 0;
    };

    class Shot final : public SerializableComponent
    {
    public:
        explicit Shot(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("x", x).Replicated();
            RegisterMember("vx", vx).Replicated();
            RegisterMember("life", life).Replicated();
            RegisterMember("damage", damage).Replicated();
            RegisterMember("shooter", shooter).Replicated();
        }

        [[nodiscard]] std::string GetTypeName() const override { return "RbShot"; }

        float x = 0.0f;
        float vx = 0.0f;
        int life = 0;
        int damage = 0;
        int shooter = 0;
    };

    /// Registers the two components with the component registry (once per process)
    inline void RegisterTestComponents()
    {
        static const bool done = []
        {
            ComponentRegistry::Instance().Register(
                "RbAvatar", [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<Avatar>(go); }, false);
            ComponentRegistry::Instance().Register(
                "RbShot", [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<Shot>(go); }, false);
            return true;
        }();
        (void)done;
    }

    /// The prefab "RbShotPrefab" (a NetworkObject and a Shot on the root) registered for this object's life
    class ShotPrefab
    {
    public:
        ShotPrefab()
        {
            RegisterTestComponents();
            const GameObject::Ptr root = GameObject::Create("ShotPrefab");
            root->AddComponent<NetworkObject>();
            root->AddComponent<Shot>();
            EXPECT_TRUE(PrefabManager::RegisterPrefab(kShotPrefab, root));
        }
        ~ShotPrefab() { PrefabManager::UnregisterPrefab(kShotPrefab); }
        ShotPrefab(const ShotPrefab &) = delete;
        ShotPrefab &operator=(const ShotPrefab &) = delete;
    };

    /// fire (button) and move (signed 8-bit Vector2, range 1)
    inline InputLayout MakeLayout()
    {
        auto created = InputLayout::Create({
            {"move", InputControlKind::Vector2, 8, 1.0f, true},
            {"fire", InputControlKind::Button, 1, 1.0f, false},
        });
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? *created : InputLayout{};
    }

    /// The packed input of (moveX, moveY, fire); values go through the layout, so they are the steps it can hold
    inline N2Net::LockstepInput MakeInput(const InputLayout &layout, const float moveX, const float moveY, const bool fire)
    {
        // Control order is by name: fire, then move
        const std::vector<float> values{fire ? 1.0f : 0.0f, moveX, moveY};
        const auto packed = layout.Pack(values);
        EXPECT_TRUE(packed.has_value());
        const auto input = N2Net::LockstepInput::FromBytes(*packed);
        EXPECT_TRUE(input.has_value());
        return input.value_or(N2Net::LockstepInput{});
    }

    /// The scripted input of `slot` at `tick`, the same on every peer: it changes every few ticks, which is what makes
    /// "repeat the last input" a wrong guess some of the time
    inline N2Net::LockstepInput ScriptedInput(const InputLayout &layout, const std::uint32_t slot, const std::uint32_t tick)
    {
        const std::uint32_t phase = (tick + slot * 3) / 5;
        const float moveX = static_cast<float>((phase * 7 + slot * 3) % 9) / 4.0f - 1.0f;
        const float moveY = static_cast<float>((phase * 5 + slot) % 7) / 3.0f - 1.0f;
        const bool fire = (phase + slot) % 4 == 0 && (tick % 5) == 1;
        return MakeInput(layout, moveX, moveY, fire);
    }

    /// The game: `slots` avatars (net ids 1..slots), shots spawned on fire and despawned when they run out
    class Arena
    {
    public:
        Arena(const std::uint32_t slotCount, const InputLayout &inputLayout) : slots(slotCount), layout(inputLayout), rollback(world, nullptr)
        {
            RegisterTestComponents();
            scene = Scene::Create("RbArena");
            for (std::uint32_t slot = 0; slot < slotCount; ++slot)
            {
                const GameObject::Ptr gameObject = GameObject::Create("Avatar" + std::to_string(slot));
                gameObject->SetUUID(TestUuid(10 + slot * 10));
                gameObject->AddComponent<NetworkObject>();
                gameObject->AddComponent<Avatar>();
                scene->AddRootGameObject(gameObject);
            }
            EXPECT_TRUE(world.BindScene(*scene).has_value());
            rollback.AddRandomStream(rng);
            scene->ProcessAttachQueue();
        }

        /// One tick of the game
        bool Step(const N2Net::TickInputs &inputs)
        {
            const std::uint32_t tick = inputs.tick;
            for (std::uint32_t slot = 0; slot < slots; ++slot)
            {
                NetworkObject *const object = world.Find(NetworkId{slot + 1});
                if (object == nullptr)
                {
                    return false;
                }
                Avatar &avatar = *object->GetGameObject().GetComponent<Avatar>();
                const auto values = layout.Unpack(inputs.inputs[slot].Bytes());
                if (!values.has_value())
                {
                    return false;
                }
                avatar.x += (*values)[1] * 0.37f;
                avatar.y += (*values)[2] * 0.29f;
                if ((*values)[0] > 0.5f && static_cast<int>(tick) - avatar.lastFire >= 3)
                {
                    avatar.lastFire = static_cast<int>(tick);
                    auto spawned = rollback.Spawn(kShotPrefab, slot + 1, tick);
                    if (!spawned.has_value())
                    {
                        return false;
                    }
                    Shot &shot = *(*spawned)->GetGameObject().GetComponent<Shot>();
                    shot.x = avatar.x;
                    shot.vx = slot % 2 == 0 ? 0.5f : -0.5f;
                    shot.life = 4 + rng.NextInt(0, 3);
                    shot.damage = rng.NextInt(1, 6);
                    shot.shooter = static_cast<int>(slot);
                    events->Emit((*spawned)->GetNetworkId().value, kEventFire, {});
                }
            }
            std::vector<std::uint32_t> shotIds;
            for (const auto &entry : world.Objects())
            {
                if (entry.second->IsSpawned())
                {
                    shotIds.push_back(entry.first);
                }
            }
            for (const std::uint32_t id : shotIds)
            {
                NetworkObject &object = *world.Find(NetworkId{id});
                Shot &shot = *object.GetGameObject().GetComponent<Shot>();
                shot.x += shot.vx;
                if (--shot.life <= 0)
                {
                    NetworkObject *const owner = world.Find(NetworkId{static_cast<std::uint32_t>(shot.shooter) + 1});
                    if (owner != nullptr)
                    {
                        owner->GetGameObject().GetComponent<Avatar>()->score += shot.damage;
                    }
                    const std::uint8_t payload[1] = {static_cast<std::uint8_t>(shot.damage)};
                    events->Emit(id, kEventHit, payload);
                    rollback.Despawn(NetworkId{id}, tick);
                }
            }
            scene->ProcessAttachQueue();
            scene->ProcessDestroyed();
            return true;
        }

        [[nodiscard]] std::uint64_t Root(const std::uint32_t tick = 0)
        {
            const auto tree = BuildStateHashTree(world, rollback.Layouts(), tick);
            EXPECT_TRUE(tree.has_value());
            return tree.has_value() ? tree->Root() : 0;
        }

        /// Every live object's replicated fields as exact JSON text (unquantised), ascending by net id
        [[nodiscard]] std::string Dump()
        {
            std::string text;
            for (const auto &entry : world.Objects())
            {
                GameObject &gameObject = entry.second->GetGameObject();
                const ObjectLayout &objectLayout = rollback.Layouts().Get(NetworkId{entry.first}, gameObject);
                const ObjectValues values = CaptureObjectValues(objectLayout, gameObject);
                text += "#" + std::to_string(entry.first) + ":";
                for (const auto &field : values.fields)
                {
                    text += field.has_value() ? field->dump() : std::string("null");
                    text += ",";
                }
                text += "\n";
            }
            text += "rng=" + std::to_string(rng.State());
            return text;
        }

        std::uint32_t slots;
        InputLayout layout;
        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        RollbackWorld rollback;
        Math::DeterministicRandom rng{4242};
        RollbackEventQueue *events = nullptr;
    };
}
