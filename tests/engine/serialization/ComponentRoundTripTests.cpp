#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/io/Resources.hpp"
#include "engine/physics/Rigidbody.hpp"

using namespace N2Engine;
using json = nlohmann::json;

// Every scene load rebuilds the scene from its JSON, so a component type that isn't registered (or
// doesn't save its settings) is lost or reset whenever a scene is loaded

namespace
{
    // Saves one root object in a scene and loads the scene back, as SceneManager does
    GameObject::Ptr RoundTrip(const GameObject::Ptr &go, std::unique_ptr<Scene> &loadedScene)
    {
        const auto scene = Scene::Create("RoundTrip_" + go->GetName());
        scene->AddRootGameObject(go);
        loadedScene = Scene::FromJSON(scene->Serialize());
        if (!loadedScene)
        {
            return nullptr;
        }
        return loadedScene->FindGameObject(go->GetName());
    }
}

TEST(ComponentRoundTripTest, RigidbodyAndItsSettingsSurviveASceneLoad)
{
    const auto go = GameObject::Create("Body");
    auto *rb = go->AddComponent<Physics::Rigidbody>();
    rb->SetBodyType(Physics::BodyType::Kinematic);
    rb->SetMass(4.5f);
    rb->SetGravityEnabled(false);

    std::unique_ptr<Scene> scene;
    const auto loaded = RoundTrip(go, scene);
    ASSERT_NE(loaded, nullptr);

    const auto *loadedRb = loaded->GetComponent<Physics::Rigidbody>();
    ASSERT_NE(loadedRb, nullptr) << "Rigidbody wasn't registered, so loading dropped it";
    EXPECT_EQ(loadedRb->GetBodyType(), Physics::BodyType::Kinematic);
    EXPECT_FLOAT_EQ(loadedRb->GetMass(), 4.5f); // no body yet, so this is the saved value
    EXPECT_FALSE(loadedRb->IsGravityEnabled());
}

// Nothing here plays or loads sample data, so no OpenAL context is needed
TEST(ComponentRoundTripTest, AudioSourceAndItsSettingsSurviveASceneLoad)
{
    // A clip registered at runtime; the saved reference is its UUID
    const auto clip = std::make_shared<Audio::AudioClip>();
    IO::Resources::Instance().RegisterAsset(clip);

    const auto go = GameObject::Create("Speaker");
    auto *source = go->AddComponent<Audio::AudioSource>();
    source->SetClip(clip);
    source->SetVolume(0.25f);
    source->SetPitch(1.5f);
    source->SetLoop(true);
    source->SetSpatial(false);
    source->SetMixerGroup("Music");
    source->SetPlayOnAwake(true);
    source->SetMinDistance(2.0f);
    source->SetMaxDistance(40.0f);
    source->SetRolloffFactor(0.5f);

    std::unique_ptr<Scene> scene;
    const auto loaded = RoundTrip(go, scene);
    ASSERT_NE(loaded, nullptr);

    const auto *loadedSource = loaded->GetComponent<Audio::AudioSource>();
    ASSERT_NE(loadedSource, nullptr) << "AudioSource wasn't registered, so loading dropped it";
    EXPECT_EQ(loadedSource->GetClip(), clip);
    EXPECT_FLOAT_EQ(loadedSource->GetVolume(), 0.25f);
    EXPECT_FLOAT_EQ(loadedSource->GetPitch(), 1.5f);
    EXPECT_TRUE(loadedSource->GetLoop());
    EXPECT_FALSE(loadedSource->GetSpatial());
    EXPECT_EQ(loadedSource->GetMixerGroup(), "Music");
    EXPECT_TRUE(loadedSource->GetPlayOnAwake());
    EXPECT_FLOAT_EQ(loadedSource->GetMinDistance(), 2.0f);
    EXPECT_FLOAT_EQ(loadedSource->GetMaxDistance(), 40.0f);
    EXPECT_FLOAT_EQ(loadedSource->GetRolloffFactor(), 0.5f);

    IO::Resources::Instance().UnregisterAsset(clip->GetUUID());
}

TEST(ComponentRoundTripTest, AudioListenerSurvivesASceneLoad)
{
    const auto go = GameObject::Create("Ears");
    go->AddComponent<Audio::AudioListener>()->SetActive(false);

    std::unique_ptr<Scene> scene;
    const auto loaded = RoundTrip(go, scene);
    ASSERT_NE(loaded, nullptr);

    const auto *listener = loaded->GetComponent<Audio::AudioListener>();
    ASSERT_NE(listener, nullptr) << "AudioListener wasn't registered, so loading dropped it";
    EXPECT_FALSE(listener->IsActive()) << "its enabled flag is saved by Component";
}

TEST(ComponentRoundTripTest, ExampleRenderersAndTheirSettingsSurviveASceneLoad)
{
    const auto go = GameObject::Create("Shapes");
    auto *cube = go->AddComponent<Example::CubeRenderer>();
    cube->SetColor(Common::Color::Cyan);
    cube->SetSize(Math::Vector3(1.0f, 2.0f, 3.0f));
    auto *sphere = go->AddComponent<Example::SphereRenderer>();
    sphere->SetColor(Common::Color::Magenta);
    sphere->SetRadius(2.5f);
    sphere->SetSubdivision(8, 12);

    std::unique_ptr<Scene> scene;
    const auto loaded = RoundTrip(go, scene);
    ASSERT_NE(loaded, nullptr);

    const auto *loadedCube = loaded->GetComponent<Example::CubeRenderer>();
    ASSERT_NE(loadedCube, nullptr) << "CubeRenderer wasn't registered, so loading dropped it";
    EXPECT_FLOAT_EQ(loadedCube->GetColor().r, Common::Color::Cyan.r);
    EXPECT_FLOAT_EQ(loadedCube->GetColor().g, Common::Color::Cyan.g);
    EXPECT_FLOAT_EQ(loadedCube->GetColor().b, Common::Color::Cyan.b);
    EXPECT_FLOAT_EQ(loadedCube->GetColor().a, Common::Color::Cyan.a);
    EXPECT_FLOAT_EQ(loadedCube->GetSize().x, 1.0f);
    EXPECT_FLOAT_EQ(loadedCube->GetSize().y, 2.0f);
    EXPECT_FLOAT_EQ(loadedCube->GetSize().z, 3.0f);

    const auto *loadedSphere = loaded->GetComponent<Example::SphereRenderer>();
    ASSERT_NE(loadedSphere, nullptr) << "SphereRenderer wasn't registered, so loading dropped it";
    EXPECT_FLOAT_EQ(loadedSphere->GetColor().r, Common::Color::Magenta.r);
    EXPECT_FLOAT_EQ(loadedSphere->GetColor().g, Common::Color::Magenta.g);
    EXPECT_FLOAT_EQ(loadedSphere->GetColor().b, Common::Color::Magenta.b);
    EXPECT_FLOAT_EQ(loadedSphere->GetRadius(), 2.5f);
    EXPECT_EQ(loadedSphere->GetLatitudeSegments(), 8u);
    EXPECT_EQ(loadedSphere->GetLongitudeSegments(), 12u);
}

TEST(ComponentRoundTripTest, UnknownComponentTypeIsReportedNotSkippedSilently)
{
    const json data = {
        {"uuid", Math::UUID::Random().ToString()},
        {"name", "HasUnknown"},
        {"components", json::array({json{{"type", "NoSuchComponent"}, {"data", json::object()}}})},
    };

    std::vector<std::string> warnings;
    const size_t id = Logger::logEvent += [&warnings](std::string_view message, Logger::LogLevel level)
    {
        if (level == Logger::LogLevel::Warn)
        {
            warnings.emplace_back(message);
        }
    };
    const auto go = GameObject::Deserialize(data);
    Logger::logEvent -= id;

    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->GetComponentCount(), 0u);
    const bool reported = std::ranges::any_of(warnings, [](const std::string &warning)
    {
        return warning.find("NoSuchComponent") != std::string::npos;
    });
    EXPECT_TRUE(reported) << "the unknown type was dropped without a word";
}
