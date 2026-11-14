// A build without the particles, soft-body, audio or UI module (N2ENGINE_USE_PARTICLES=OFF, N2ENGINE_USE_SOFTBODIES=OFF,
// N2ENGINE_USE_AUDIO=OFF, N2ENGINE_USE_UI=OFF; docs/project-modules.html). The component stays registered with every field,
// keeps its data through a save and a load, does nothing when the scene plays, warns at most once, and Lua's methods still
// exist. tests/engine/CMakeLists.txt defines N2ENGINE_<MODULE>_ENABLED on engine_tests when the module is in the build, so a
// full build compiles none of these tests (the module's own tests run instead).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/physics/SoftBody.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

#if !defined(N2ENGINE_PARTICLES_ENABLED) || !defined(N2ENGINE_SOFTBODIES_ENABLED) || !defined(N2ENGINE_AUDIO_ENABLED) ||     \
    !defined(N2ENGINE_UI_ENABLED)

using namespace N2Engine;

namespace
{
    /// Counts the warnings logged while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        [[nodiscard]] int Count(const std::string &text) const
        {
            return static_cast<int>(std::ranges::count_if(
                warnings, [&](const std::string &w) { return w.find(text) != std::string::npos; }));
        }

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };

    /// A playing headless scene, stepped by the real Application::StepTicks, and Lua for the bindings
    class LeftOutModuleTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;

        static void SetUpTestSuite() { ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize()); }

        void SetUp() override
        {
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
            SceneManager::AddScene(
                Scene::Create(std::string("LeftOut_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
            ASSERT_NE(_scene, nullptr);
        }

        void TearDown() override
        {
            Run("left_out_go = nil; left_out_component = nil");
            Scripting::LuaRuntime::Instance().GetState().collect_garbage();
            SceneManager::AddScene(Scene::Create("LeftOut_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
        }

        void Step(const int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                ASSERT_EQ(Application::GetInstance().StepTicks(1), 1u);
                _scene->ProcessDestroyed();
            }
        }

        GameObject::Ptr Make(const std::string &name)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(Math::Vector3(0.0f, 2.0f, 0.0f));
            _scene->AddRootGameObject(go);
            return go;
        }

        /// The scene saved and loaded back, as SceneManager does, and the object of that name in it
        GameObject::Ptr RoundTrip(const std::string &name)
        {
            _loaded = Scene::FromJSON(_scene->Serialize());
            return _loaded ? _loaded->FindGameObject(name) : nullptr;
        }

        static void Run(const std::string &code)
        {
            const sol::protected_function_result result =
                Scripting::LuaRuntime::Instance().GetState().safe_script(code, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error err = result;
                FAIL() << "Lua error: " << err.what() << "\nin: " << code;
            }
        }

        template <typename T>
        static T Eval(const std::string &expression)
        {
            const sol::protected_function_result result =
                Scripting::LuaRuntime::Instance().GetState().safe_script("return " + expression, sol::script_pass_on_error);
            if (!result.valid())
            {
                const sol::error err = result;
                ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
                return T{};
            }
            return result.get<T>();
        }

        std::unique_ptr<Scene> _loaded;
    };

    bool HasField(const std::string &type, const std::string &field)
    {
        const std::optional<ComponentSchema> schema = ComponentRegistry::Instance().Describe(type);
        return schema && std::ranges::any_of(schema->fields, [&](const FieldInfo &f) { return f.name == field; });
    }
}

#ifndef N2ENGINE_PARTICLES_ENABLED

using Rendering::ParticleSystem;

TEST_F(LeftOutModuleTest, AParticleSystemKeepsItsDataEmitsNothingAndWarnsAtMostOnce)
{
    WarningCapture capture;
    auto go = Make("Fx");
    auto *system = go->AddComponent<ParticleSystem>();
    system->maxParticles = 250;
    system->seed = 17;
    system->burstCount = 40;
    system->emissionRate = 500.0f;
    system->backend = Particles::ParticleBackend::CPU;
    system->startColor = Common::Color(1.0f, 0.5f, 0.25f, 1.0f);
    auto *second = Make("Fx2")->AddComponent<ParticleSystem>();

    Step(10);
    system->EmitBurst(20);
    Step(2);

    EXPECT_TRUE(system->IsPlaying()) << "playOnAwake still plays: only the simulation is missing";
    EXPECT_EQ(system->GetAliveCount(), 0u);
    EXPECT_EQ(second->GetAliveCount(), 0u);
    EXPECT_TRUE(system->GetActiveBackendName().empty());
    std::vector<Math::Vector4> particles{Math::Vector4()};
    system->GetParticles(particles);
    EXPECT_TRUE(particles.empty());
    system->Stop();
    system->Clear();
    EXPECT_FALSE(system->IsPlaying());
    // Once per process: earlier tests in this binary may have logged it already, but two systems never log it twice
    EXPECT_LE(capture.Count("no particles module"), 1);

    const auto back = RoundTrip("Fx");
    ASSERT_NE(back, nullptr);
    const auto *loaded = back->GetComponent<ParticleSystem>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->maxParticles, 250);
    EXPECT_EQ(loaded->seed, 17);
    EXPECT_EQ(loaded->burstCount, 40);
    EXPECT_FLOAT_EQ(loaded->emissionRate, 500.0f);
    EXPECT_EQ(loaded->backend, Particles::ParticleBackend::CPU);
    EXPECT_FLOAT_EQ(loaded->startColor.g, 0.5f);
}

TEST_F(LeftOutModuleTest, ParticleSystemIsRegisteredWithEveryField)
{
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("ParticleSystem"));
    for (const char *field : {"backend", "playOnAwake", "maxParticles", "seed", "pbdMaterial", "emissionRate", "burstCount",
                              "shape", "lifetimeMin", "startSize", "startColor", "gravityScale", "collideWithGround",
                              "blendMode", "texture"})
    {
        EXPECT_TRUE(HasField("ParticleSystem", field)) << field;
    }
}

TEST_F(LeftOutModuleTest, ParticleSystemLuaMethodsExistAndDoNothing)
{
    Run(R"(
        left_out_go = GameObject.Create("LuaFx")
        left_out_go:CreatePositionable()
        SceneManager.GetCurrentScene():AddRootGameObject(left_out_go)
        left_out_component = left_out_go:AddComponent("ParticleSystem")
        left_out_component:SetMaxParticles(42)
        left_out_component:SetEmissionRate(100)
        left_out_component:Play()
        left_out_component:EmitBurst(10)
    )");
    Step(5);
    Run("left_out_component:EmitBurst(10); left_out_component:Stop(); left_out_component:Clear()");

    EXPECT_EQ(Eval<int>("left_out_component:GetAliveCount()"), 0);
    EXPECT_TRUE(Eval<bool>("left_out_component:GetActiveBackend() == nil"));
    EXPECT_EQ(Eval<int>("left_out_component:GetMaxParticles()"), 42);
    EXPECT_FALSE(Eval<bool>("left_out_component:IsPlaying()"));
    EXPECT_TRUE(Eval<bool>("left_out_go:GetComponent('ParticleSystem') == left_out_component"));
}

#endif // particles

#ifndef N2ENGINE_SOFTBODIES_ENABLED

using Physics::SoftBody;
using Rendering::MeshRenderer;

namespace
{
    /// One triangle: enough for a MeshRenderer to have a mesh a SoftBody would start from
    std::shared_ptr<Rendering::Mesh> MakeTriangle()
    {
        Renderer::Common::MeshData data;
        for (int i = 0; i < 3; ++i)
        {
            Renderer::Common::Vertex vertex{};
            vertex.position[0] = i == 1 ? 1.0f : 0.0f;
            vertex.position[1] = i == 2 ? 1.0f : 0.0f;
            vertex.normal[2] = 1.0f;
            data.vertices.push_back(vertex);
        }
        data.indices = {0, 1, 2};
        return Rendering::Mesh::Create(data);
    }
}

TEST_F(LeftOutModuleTest, ASoftBodyKeepsItsDataLeavesTheMeshAloneAndWarnsAtMostOnce)
{
    WarningCapture capture;
    auto go = Make("Jelly");
    auto *renderer = go->AddComponent<MeshRenderer>();
    const auto mesh = MakeTriangle();
    ASSERT_NE(mesh, nullptr);
    renderer->SetMesh(mesh);
    auto *body = go->AddComponent<SoftBody>();
    body->resolution = 6;
    body->stiffness = 2.5e4f;
    body->collideWithGround = true;
    body->groundHeight = -1.0f;
    body->backend = SoftBodies::SoftBodyBackend::CPU;
    auto other = Make("Jelly2");
    other->AddComponent<MeshRenderer>()->SetMesh(MakeTriangle());
    other->AddComponent<SoftBody>();

    Step(10);
    body->AddVelocity(Math::Vector3(0.0f, 5.0f, 0.0f));
    body->Reset();
    Step(2);

    EXPECT_TRUE(body->GetActiveBackendName().empty());
    EXPECT_EQ(body->GetTetMesh(), nullptr);
    EXPECT_EQ(body->GetRuntimeMesh(), nullptr);
    EXPECT_EQ(renderer->GetDeformedMesh(), nullptr) << "the MeshRenderer draws its own mesh";
    EXPECT_EQ(renderer->GetMesh(), mesh);
    EXPECT_FLOAT_EQ(go->GetPositionable()->GetPosition().y, 2.0f);
    EXPECT_LE(capture.Count("no soft-body module"), 1);

    const auto back = RoundTrip("Jelly");
    ASSERT_NE(back, nullptr);
    const auto *loaded = back->GetComponent<SoftBody>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->resolution, 6);
    EXPECT_FLOAT_EQ(loaded->stiffness, 2.5e4f);
    EXPECT_TRUE(loaded->collideWithGround);
    EXPECT_FLOAT_EQ(loaded->groundHeight, -1.0f);
    EXPECT_EQ(loaded->backend, SoftBodies::SoftBodyBackend::CPU);
}

TEST_F(LeftOutModuleTest, SoftBodyIsRegisteredWithEveryField)
{
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("SoftBody"));
    for (const char *field : {"backend", "resolution", "density", "stiffness", "poisson", "damping", "friction",
                              "solverIterations", "collideWithGround", "groundHeight", "materialModel", "selfCollision"})
    {
        EXPECT_TRUE(HasField("SoftBody", field)) << field;
    }
}

TEST_F(LeftOutModuleTest, SoftBodyLuaMethodsExistAndDoNothing)
{
    Run(R"(
        left_out_go = GameObject.Create("LuaJelly")
        left_out_go:CreatePositionable()
        SceneManager.GetCurrentScene():AddRootGameObject(left_out_go)
        left_out_go:AddComponent("CubeRenderer")
        left_out_component = left_out_go:AddComponent("SoftBody")
        left_out_component:SetStiffness(12345)
    )");
    Step(5);
    Run("left_out_component:AddVelocity(Vector3(0, 3, 0)); left_out_component:Reset()");

    EXPECT_EQ(Eval<std::string>("left_out_component:GetActiveBackendName()"), "");
    EXPECT_FLOAT_EQ(Eval<float>("left_out_component:GetStiffness()"), 12345.0f);
    EXPECT_TRUE(Eval<bool>("left_out_go:GetComponent('SoftBody') == left_out_component"));
}

#endif // soft bodies

#ifndef N2ENGINE_AUDIO_ENABLED

using Audio::AudioSource;
using Audio::AudioSystem;

TEST_F(LeftOutModuleTest, AnAudioSourceKeepsItsDataPlaysNothingAndWarnsAtMostOnce)
{
    WarningCapture capture;
    EXPECT_FALSE(AudioSystem::IsInBuild());
    EXPECT_FALSE(AudioSystem::Instance().Initialize()) << "the audio system never starts";
    EXPECT_FALSE(AudioSystem::Instance().IsInitialized());

    auto go = Make("Speaker");
    auto *source = go->AddComponent<AudioSource>();
    source->SetVolume(0.25f);
    source->SetPitch(1.5f);
    source->SetLoop(true);
    source->SetMixerGroup("Music");
    source->SetPlayOnAwake(true);
    Make("Speaker2")->AddComponent<AudioSource>();
    Make("Ears")->AddComponent<Audio::AudioListener>();

    Step(5);
    source->Play();
    Step(2);

    EXPECT_FALSE(source->IsPlaying());
    EXPECT_EQ(source->GetSourceHandle(), 0u) << "no OpenAL source was ever made";
    EXPECT_FALSE(AudioSystem::Instance().PlayOneShot(nullptr).IsValid());
    // Once per process: earlier tests in this binary may have logged it already, but two sources never log it twice
    EXPECT_LE(capture.Count("no audio module"), 1);

    const auto back = RoundTrip("Speaker");
    ASSERT_NE(back, nullptr);
    const auto *loaded = back->GetComponent<AudioSource>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->GetVolume(), 0.25f);
    EXPECT_FLOAT_EQ(loaded->GetPitch(), 1.5f);
    EXPECT_TRUE(loaded->GetLoop());
    EXPECT_EQ(loaded->GetMixerGroup(), "Music");
    EXPECT_TRUE(loaded->GetPlayOnAwake());
}

TEST_F(LeftOutModuleTest, AudioComponentsAreRegisteredWithEveryField)
{
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("AudioSource"));
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("AudioListener"));
    for (const char *field : {"_volume", "_pitch", "_loop", "_spatial", "_playOnAwake", "_minDistance", "_maxDistance",
                              "_rolloffFactor", "_mixerGroup", "_clip"})
    {
        EXPECT_TRUE(HasField("AudioSource", field)) << field;
    }
}

TEST_F(LeftOutModuleTest, AudioLuaMethodsExistAndDoNothing)
{
    Run(R"(
        left_out_go = GameObject.Create("LuaSpeaker")
        left_out_go:CreatePositionable()
        SceneManager.GetCurrentScene():AddRootGameObject(left_out_go)
        left_out_component = left_out_go:AddComponent("AudioSource")
        left_out_component:SetVolume(0.5)
        left_out_component:Play()
        Audio.SetMasterVolume(0.75)
    )");
    Step(3);

    EXPECT_FALSE(Eval<bool>("left_out_component:IsPlaying()"));
    EXPECT_FLOAT_EQ(Eval<float>("left_out_component:GetVolume()"), 0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("Audio.GetMasterVolume()"), 0.75f);
    EXPECT_TRUE(Eval<bool>("left_out_go:GetComponent('AudioSource') == left_out_component"));
    Run("Audio.SetMasterVolume(1.0)");
}

#endif // audio

#ifndef N2ENGINE_UI_ENABLED

TEST_F(LeftOutModuleTest, UIKeepsItsDataDrawsNothingIsNeverHitAndWarnsAtMostOnce)
{
    WarningCapture capture;
    EXPECT_FALSE(UI::UISystem::IsInBuild());
    auto canvas = UI::UISystem::CreateCanvas("HUD");
    canvas->GetComponent<UI::Canvas>()->SetSortOrder(7);
    auto button = UI::UISystem::CreateButton("Play", "Start");
    button->GetComponent<UI::RectTransform>()->SetSizeDelta(Math::Vector2{200.0f, 50.0f});
    canvas->AddChild(button, false);
    _scene->AddRootGameObject(canvas);
    auto world = UI::UISystem::CreateCanvas("Sign", UI::CanvasRenderMode::WorldSpace);
    world->AddChild(UI::UISystem::CreateText("Label", "Hello"), false);
    _scene->AddRootGameObject(world);

    Step(3);
    const Vector2i viewport{800, 600};
    EXPECT_TRUE(UI::UISystem::CollectGraphics(*_scene, viewport).empty()) << "nothing is laid out";
    EXPECT_TRUE(UI::UISystem::CollectWorldCanvasGraphics(*world->GetComponent<UI::Canvas>()).empty());
    EXPECT_EQ(UI::UISystem::HitTestWindowPoint(*_scene, Math::Vector2{400.0f, 300.0f}, viewport), nullptr);
    EXPECT_EQ(UI::UISystem::HitTestScreenPoint(*_scene, nullptr, Math::Vector2{400.0f, 300.0f}, viewport, 0xFFFFFFFFu),
              nullptr);
    EXPECT_LE(capture.Count("no UI module"), 1);

    const auto back = RoundTrip("HUD");
    ASSERT_NE(back, nullptr);
    ASSERT_NE(back->GetComponent<UI::Canvas>(), nullptr);
    EXPECT_EQ(back->GetComponent<UI::Canvas>()->GetSortOrder(), 7);
    const auto loadedButton = back->FindChild("Play");
    ASSERT_NE(loadedButton, nullptr);
    EXPECT_NE(loadedButton->GetComponent<UI::Button>(), nullptr);
    EXPECT_NE(loadedButton->GetComponent<UI::Image>(), nullptr);
    ASSERT_NE(loadedButton->GetComponent<UI::RectTransform>(), nullptr);
    EXPECT_FLOAT_EQ(loadedButton->GetComponent<UI::RectTransform>()->GetSizeDelta().x, 200.0f);
    EXPECT_FLOAT_EQ(loadedButton->GetComponent<UI::RectTransform>()->GetSizeDelta().y, 50.0f);
    const auto label = loadedButton->FindChild("Label");
    ASSERT_NE(label, nullptr);
    ASSERT_NE(label->GetComponent<UI::UIText>(), nullptr);
    EXPECT_EQ(label->GetComponent<UI::UIText>()->GetText(), "Start");
}

TEST_F(LeftOutModuleTest, UIComponentsAreRegisteredWithEveryField)
{
    for (const char *type : {"Canvas", "RectTransform", "Image", "UIText", "Button"})
    {
        EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered(type)) << type;
    }
    EXPECT_TRUE(HasField("Canvas", "sortOrder"));
    EXPECT_TRUE(HasField("Canvas", "renderMode"));
    EXPECT_TRUE(HasField("RectTransform", "sizeDelta"));
    EXPECT_TRUE(HasField("Image", "color"));
    EXPECT_TRUE(HasField("UIText", "text"));
    EXPECT_TRUE(HasField("UIText", "fontSize"));
    EXPECT_TRUE(HasField("Button", "interactable"));
}

TEST_F(LeftOutModuleTest, UILuaMethodsExist)
{
    Run(R"(
        left_out_go = UI.CreateCanvas("LuaHUD")
        SceneManager.GetCurrentScene():AddRootGameObject(left_out_go)
        local text = UI.CreateText("LuaLabel", "Score 0")
        left_out_go:AddChild(text)
        left_out_component = text:GetComponent("UIText")
        left_out_component:SetText("Score 10")
        left_out_go:GetComponent("Canvas"):SetSortOrder(3)
    )");
    Step(3);

    EXPECT_EQ(Eval<std::string>("left_out_component:GetText()"), "Score 10");
    EXPECT_EQ(Eval<int>("left_out_go:GetComponent('Canvas'):GetSortOrder()"), 3);
}

#endif // UI

#endif // any module left out

