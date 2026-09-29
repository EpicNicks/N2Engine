#include <gtest/gtest.h>

#include <math/Vector3.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;

// Scene::Clear detaches root objects with SetScene(nullptr), which used to dereference the
// null scene for every component, so leaving any scene with components crashed.
TEST(SceneSwitchTest, LeavingSceneWithComponentsDoesNotCrash)
{
    SceneManager::AddScene(Scene::Create("SceneSwitchFrom"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);

    const auto parent = GameObject::Create("WithComponent");
    parent->AddComponent<Physics::BoxCollider>()->SetSize(Math::Vector3::One);
    const auto child = GameObject::Create("ChildWithComponent");
    child->AddComponent<Physics::BoxCollider>();
    parent->AddChild(child);
    SceneManager::GetCurSceneRef().AddRootGameObject(parent);

    SceneManager::AddScene(Scene::Create("SceneSwitchTo"), true);
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "SceneSwitchTo");
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("WithComponent"), nullptr);
}
