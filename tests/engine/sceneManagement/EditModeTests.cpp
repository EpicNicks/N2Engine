#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Light.hpp"

using namespace N2Engine;

// What the editor host needs of the engine (#6, E4): objects in an order that can be changed, lights found in a scene
// whose components never attached, and no OnEnable/OnDisable for a scene that was never run.
namespace
{
    // Counts the enable/disable callbacks it gets
    struct EnableCounts
    {
        int enable = 0;
        int disable = 0;
    };

    class EnableProbe final : public Component
    {
    public:
        explicit EnableProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "EnableProbe"; }

        void OnEnable() override { ++counts->enable; }
        void OnDisable() override { ++counts->disable; }

        std::shared_ptr<EnableCounts> counts = std::make_shared<EnableCounts>();
    };

    std::vector<std::string> Names(const std::vector<GameObject::Ptr> &objects)
    {
        std::vector<std::string> names;
        for (const auto &object : objects)
        {
            names.push_back(object->GetName());
        }
        return names;
    }

    using Names_t = std::vector<std::string>;
}

// ==================== Sibling order ====================

TEST(SiblingIndexTest, ChildrenKeepTheirOrderAndMoveToAnIndex)
{
    auto scene = Scene::Create("SiblingIndexChildren");
    const auto parent = GameObject::Create("Parent");
    scene->AddRootGameObject(parent);
    const auto a = GameObject::Create("A");
    const auto b = GameObject::Create("B");
    const auto c = GameObject::Create("C");
    const auto d = GameObject::Create("D");
    for (const auto &child : {a, b, c, d})
    {
        parent->AddChild(child);
    }
    EXPECT_EQ(Names(parent->GetChildren()), (Names_t{"A", "B", "C", "D"}));
    EXPECT_EQ(c->GetSiblingIndex(), 2u);

    d->SetSiblingIndex(0);
    EXPECT_EQ(Names(parent->GetChildren()), (Names_t{"D", "A", "B", "C"}));
    d->SetSiblingIndex(2);
    EXPECT_EQ(Names(parent->GetChildren()), (Names_t{"A", "B", "D", "C"}));
    a->SetSiblingIndex(3);
    EXPECT_EQ(Names(parent->GetChildren()), (Names_t{"B", "D", "C", "A"}));
    EXPECT_EQ(a->GetSiblingIndex(), 3u);
    EXPECT_EQ(b->GetSiblingIndex(), 0u);
}

TEST(SiblingIndexTest, AnIndexPastTheLastIsTheLast)
{
    auto scene = Scene::Create("SiblingIndexClamp");
    const auto a = GameObject::Create("A");
    const auto b = GameObject::Create("B");
    const auto c = GameObject::Create("C");
    scene->AddRootGameObjects({a, b, c});

    a->SetSiblingIndex(99);
    EXPECT_EQ(Names(scene->GetRootGameObjects()), (Names_t{"B", "C", "A"}));
    EXPECT_EQ(a->GetSiblingIndex(), 2u);

    // Where it already is: nothing moves
    a->SetSiblingIndex(2);
    EXPECT_EQ(Names(scene->GetRootGameObjects()), (Names_t{"B", "C", "A"}));
}

TEST(SiblingIndexTest, RootsOfASceneAreSiblings)
{
    auto scene = Scene::Create("SiblingIndexRoots");
    const auto a = GameObject::Create("A");
    const auto b = GameObject::Create("B");
    const auto c = GameObject::Create("C");
    scene->AddRootGameObjects({a, b, c});

    c->SetSiblingIndex(0);
    EXPECT_EQ(Names(scene->GetRootGameObjects()), (Names_t{"C", "A", "B"}));
    EXPECT_EQ(b->GetSiblingIndex(), 2u);
}

TEST(SiblingIndexTest, MovingChangesNeitherTheTransformNorTheActiveState)
{
    auto scene = Scene::Create("SiblingIndexUnchanged");
    const auto parent = GameObject::Create("Parent");
    parent->CreatePositionable();
    scene->AddRootGameObject(parent);
    const auto a = GameObject::Create("A");
    const auto b = GameObject::Create("B");
    for (const auto &child : {a, b})
    {
        child->CreatePositionable();
        parent->AddChild(child);
    }
    b->GetPositionable()->SetLocalPosition(Math::Vector3(1.0f, 2.0f, 3.0f));
    b->SetActive(false);

    b->SetSiblingIndex(0);
    EXPECT_EQ(Names(parent->GetChildren()), (Names_t{"B", "A"}));
    EXPECT_EQ(b->GetPositionable()->GetLocalPosition(), Math::Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_FALSE(b->IsActive());
    EXPECT_EQ(b->GetParent(), parent);
}

TEST(SiblingIndexTest, AnObjectInNoSceneAndWithNoParentHasNoSiblings)
{
    const auto loose = GameObject::Create("Loose");
    EXPECT_EQ(loose->GetSiblingIndex(), 0u);
    loose->SetSiblingIndex(5); // nothing to do, and no crash
    EXPECT_EQ(loose->GetSiblingIndex(), 0u);
}

// ==================== Edit-mode lighting ====================

TEST(EditModeLightingTest, ALightThatNeverAttachedLightsTheSceneOnlyInEditMode)
{
    auto scene = Scene::Create("EditModeLighting");
    const auto lamp = GameObject::Create("Lamp");
    auto *light = lamp->AddComponent<Rendering::Light>();
    light->intensity = 2.0f;
    scene->AddRootGameObject(lamp);
    // Nothing calls ProcessAttachQueue, as in the editor host

    // Not in edit mode: the light never attached, so the scene gets the default light
    const auto game = scene->CollectLighting();
    ASSERT_EQ(game.directionalLights.size(), 1u);
    EXPECT_FLOAT_EQ(game.directionalLights[0].intensity, 0.8f);

    scene->SetEditMode(true);
    EXPECT_TRUE(scene->IsEditMode());
    const auto edit = scene->CollectLighting();
    ASSERT_EQ(edit.directionalLights.size(), 1u);
    EXPECT_FLOAT_EQ(edit.directionalLights[0].intensity, 2.0f) << "this light, not the default one";
}

TEST(EditModeLightingTest, EveryActiveLightInTheHierarchyCountsAndInactiveOnesDont)
{
    auto scene = Scene::Create("EditModeLightingHierarchy");
    scene->SetEditMode(true);

    const auto root = GameObject::Create("Root");
    const auto sun = GameObject::Create("Sun");
    sun->AddComponent<Rendering::Light>()->type = Rendering::LightType::Directional;
    root->AddChild(sun);

    const auto bulb = GameObject::Create("Bulb");
    bulb->AddComponent<Rendering::Light>()->type = Rendering::LightType::Point;
    root->AddChild(bulb);

    const auto hidden = GameObject::Create("Hidden");
    hidden->AddComponent<Rendering::Light>()->type = Rendering::LightType::Spot;
    hidden->SetActive(false);
    root->AddChild(hidden);

    const auto offLight = GameObject::Create("Off");
    offLight->AddComponent<Rendering::Light>()->type = Rendering::LightType::Point;
    offLight->GetComponent<Rendering::Light>()->SetActive(false);
    root->AddChild(offLight);

    scene->AddRootGameObject(root);

    auto lighting = scene->CollectLighting();
    EXPECT_EQ(lighting.directionalLights.size(), 1u);
    EXPECT_EQ(lighting.pointLights.size(), 1u) << "the disabled light isn't counted";
    EXPECT_EQ(lighting.spotLights.size(), 0u) << "the light of an inactive object isn't counted";

    // Deactivating the parent switches the whole subtree off: the default light is all that is left
    root->SetActive(false);
    lighting = scene->CollectLighting();
    EXPECT_EQ(lighting.pointLights.size(), 0u);
    ASSERT_EQ(lighting.directionalLights.size(), 1u);
    EXPECT_FLOAT_EQ(lighting.directionalLights[0].intensity, 0.8f);
}

TEST(EditModeLightingTest, ASourceCanBeChosenWhateverTheMode)
{
    auto scene = Scene::Create("EditModeLightingSource");
    const auto lamp = GameObject::Create("Lamp");
    lamp->AddComponent<Rendering::Light>()->intensity = 3.0f;
    scene->AddRootGameObject(lamp);

    EXPECT_FLOAT_EQ(scene->CollectLighting(Scene::LightingSource::Hierarchy).directionalLights[0].intensity, 3.0f);
    EXPECT_FLOAT_EQ(scene->CollectLighting(Scene::LightingSource::Attached).directionalLights[0].intensity, 0.8f);
}

// ==================== No enable/disable callbacks in edit mode ====================

TEST(EditModeCallbacksTest, ASceneThatRunsGetsOnDisableAndOnEnable)
{
    auto scene = Scene::Create("EditModeCallbacksGame");
    const auto object = GameObject::Create("Object");
    const auto counts = object->AddComponent<EnableProbe>()->counts;
    scene->AddRootGameObject(object);

    object->SetActive(false);
    EXPECT_EQ(counts->disable, 1);
    object->SetActive(true);
    EXPECT_EQ(counts->enable, 1);
}

TEST(EditModeCallbacksTest, ASceneOpenedForEditingGetsNeither)
{
    auto scene = Scene::Create("EditModeCallbacksEdit");
    scene->SetEditMode(true);
    const auto object = GameObject::Create("Object");
    const auto counts = object->AddComponent<EnableProbe>()->counts;
    scene->AddRootGameObject(object);

    object->SetActive(false);
    EXPECT_FALSE(object->IsActive()) << "the flag still changes";
    EXPECT_FALSE(object->IsActiveInHierarchy());
    object->SetActive(true);
    EXPECT_TRUE(object->IsActiveInHierarchy());
    EXPECT_EQ(counts->enable, 0);
    EXPECT_EQ(counts->disable, 0);
}

TEST(EditModeCallbacksTest, ReparentingUnderAnInactiveObjectGetsNeitherEither)
{
    auto scene = Scene::Create("EditModeCallbacksReparent");
    scene->SetEditMode(true);
    const auto off = GameObject::Create("Off");
    off->SetActive(false);
    const auto object = GameObject::Create("Object");
    const auto counts = object->AddComponent<EnableProbe>()->counts;
    scene->AddRootGameObjects({off, object});

    object->SetParent(off);
    EXPECT_FALSE(object->IsActiveInHierarchy()) << "it is inactive under its parent all the same";
    object->SetParent(nullptr);
    EXPECT_TRUE(object->IsActiveInHierarchy());
    EXPECT_EQ(counts->enable, 0);
    EXPECT_EQ(counts->disable, 0);
}
