#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <nlohmann/json.hpp>

#include "FakePhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/Color.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/FieldInfo.hpp"

// The ParticleSystem component without a renderer: registration and reflection, save and load, the emitter's
// play/stop/clear/burst rules, the maxParticles cap, backend selection and its fallback warning, and the freeze
// while paused. The fixed step is driven by calling OnFixedUpdate, as Scene::FixedUpdate does.

using namespace N2Engine;
using json = nlohmann::json;
using Rendering::ParticleSystem;

namespace
{
    /// Collects the warnings logged while it lives
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

        /// How many warnings contain `text`
        [[nodiscard]] int Count(const std::string &text) const
        {
            return static_cast<int>(std::ranges::count_if(
                warnings, [&](const std::string &w) { return w.find(text) != std::string::npos; }));
        }

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };

    class ParticleSystemTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
        }

        void TearDown() override
        {
            _objects.clear();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
        }

        /// A new object with a ParticleSystem (never in a scene): quiet settings unless a test changes them
        ParticleSystem *Make(const std::string &name = "Sparks")
        {
            auto object = GameObject::Create(name);
            auto *system = object->AddComponent<ParticleSystem>();
            system->emissionRate = 50.0f;
            system->lifetimeMin = 10.0f;
            system->lifetimeMax = 10.0f;
            system->shapeRadius = 0.0f;
            _objects.push_back(object);
            return system;
        }

        static void Step(ParticleSystem *system, const int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                system->OnFixedUpdate();
            }
        }

        std::vector<GameObject::Ptr> _objects;
    };

    /// Saves one root object in a scene and loads the scene back, as SceneManager does
    GameObject::Ptr RoundTrip(const GameObject::Ptr &go, std::unique_ptr<Scene> &loadedScene)
    {
        const auto scene = Scene::Create("ParticleRoundTrip_" + go->GetName());
        scene->AddRootGameObject(go);
        loadedScene = Scene::FromJSON(scene->Serialize());
        if (!loadedScene)
        {
            return nullptr;
        }
        return loadedScene->FindGameObject(go->GetName());
    }
}

// ---------------------------------------------------------------- registration and reflection

TEST_F(ParticleSystemTest, IsRegisteredAndDescribesItsFields)
{
    auto &registry = ComponentRegistry::Instance();
    ASSERT_TRUE(registry.IsRegistered("ParticleSystem"));
    EXPECT_FALSE(registry.IsSingleton("ParticleSystem"));

    const std::optional<ComponentSchema> schema = registry.Describe("ParticleSystem");
    ASSERT_TRUE(schema.has_value());
    const auto find = [&](const std::string &name) -> const FieldInfo *
    {
        const auto it = std::ranges::find(schema->fields, name, &FieldInfo::name);
        return it == schema->fields.end() ? nullptr : &*it;
    };

    const FieldInfo *backend = find("backend");
    ASSERT_NE(backend, nullptr) << "the backend choice is an ordinary field";
    EXPECT_EQ(backend->kind, FieldKind::Enum);
    EXPECT_EQ(backend->enumOptions, (std::vector<std::string>{"Auto", "PhysX", "CPU"}));
    EXPECT_EQ(schema->defaults.at("backend"), "Auto");

    ASSERT_NE(find("shape"), nullptr);
    EXPECT_EQ(find("shape")->enumOptions, (std::vector<std::string>{"Cone", "Sphere"}));
    ASSERT_NE(find("blendMode"), nullptr);
    EXPECT_EQ(find("blendMode")->enumOptions, (std::vector<std::string>{"Alpha", "Additive"}));
    ASSERT_NE(find("texture"), nullptr);
    EXPECT_EQ(find("texture")->kind, FieldKind::AssetRef);
    ASSERT_NE(find("startColor"), nullptr);
    EXPECT_EQ(find("startColor")->kind, FieldKind::Color);
    ASSERT_NE(find("maxParticles"), nullptr);
    EXPECT_EQ(find("maxParticles")->kind, FieldKind::Int);
    ASSERT_NE(find("emissionRate"), nullptr);
    EXPECT_EQ(find("emissionRate")->kind, FieldKind::Float);
}

TEST_F(ParticleSystemTest, SavesItsSettingsAndNoParticleState)
{
    ParticleSystem *system = Make();
    system->backend = Particles::ParticleBackend::CPU;
    system->OnAttach();
    system->EmitBurst(12);
    ASSERT_EQ(system->GetAliveCount(), 12u);

    const json saved = system->Serialize();
    EXPECT_EQ(saved.at("backend"), "CPU");
    for (const char *runtimeKey : {"particles", "alive", "count", "handle", "playing", "positions"})
    {
        EXPECT_FALSE(saved.contains(runtimeKey)) << runtimeKey << " is runtime state, never saved";
    }

    // More particles later: the saved JSON is the same
    Step(system, 10);
    EXPECT_EQ(system->Serialize(), saved);
}

TEST_F(ParticleSystemTest, SettingsSurviveASceneLoadAndTheLoadedSystemStartsEmpty)
{
    auto go = GameObject::Create("Fountain");
    auto *system = go->AddComponent<ParticleSystem>();
    system->backend = Particles::ParticleBackend::PhysX;
    system->playOnAwake = false;
    system->maxParticles = 321;
    system->seed = 99;
    system->emissionRate = 33.5f;
    system->burstCount = 7;
    system->shape = Particles::EmitterShape::Sphere;
    system->shapeRadius = 1.25f;
    system->coneAngle = 55.0f;
    system->lifetimeMin = 0.5f;
    system->lifetimeMax = 3.0f;
    system->speedMin = 1.0f;
    system->speedMax = 6.0f;
    system->startSize = 0.4f;
    system->endSize = 0.1f;
    system->startColor = Common::Color(1.0f, 0.5f, 0.25f, 0.75f);
    system->endColor = Common::Color(0.0f, 0.0f, 1.0f, 0.0f);
    system->gravityScale = -0.5f;
    system->drag = 0.3f;
    system->collideWithGround = true;
    system->groundHeight = -2.0f;
    system->bounce = 0.8f;
    system->blendMode = Rendering::ParticleBlend::Additive;

    std::unique_ptr<Scene> scene;
    const auto loaded = RoundTrip(go, scene);
    ASSERT_NE(loaded, nullptr);
    const auto *copy = loaded->GetComponent<ParticleSystem>();
    ASSERT_NE(copy, nullptr) << "ParticleSystem wasn't registered, so loading dropped it";

    EXPECT_EQ(copy->backend, Particles::ParticleBackend::PhysX);
    EXPECT_FALSE(copy->playOnAwake);
    EXPECT_EQ(copy->maxParticles, 321);
    EXPECT_EQ(copy->seed, 99);
    EXPECT_FLOAT_EQ(copy->emissionRate, 33.5f);
    EXPECT_EQ(copy->burstCount, 7);
    EXPECT_EQ(copy->shape, Particles::EmitterShape::Sphere);
    EXPECT_FLOAT_EQ(copy->shapeRadius, 1.25f);
    EXPECT_FLOAT_EQ(copy->coneAngle, 55.0f);
    EXPECT_FLOAT_EQ(copy->lifetimeMin, 0.5f);
    EXPECT_FLOAT_EQ(copy->lifetimeMax, 3.0f);
    EXPECT_FLOAT_EQ(copy->speedMin, 1.0f);
    EXPECT_FLOAT_EQ(copy->speedMax, 6.0f);
    EXPECT_FLOAT_EQ(copy->startSize, 0.4f);
    EXPECT_FLOAT_EQ(copy->endSize, 0.1f);
    EXPECT_FLOAT_EQ(copy->startColor.r, 1.0f);
    EXPECT_FLOAT_EQ(copy->startColor.g, 0.5f);
    EXPECT_FLOAT_EQ(copy->startColor.b, 0.25f);
    EXPECT_FLOAT_EQ(copy->startColor.a, 0.75f);
    EXPECT_FLOAT_EQ(copy->endColor.b, 1.0f);
    EXPECT_FLOAT_EQ(copy->endColor.a, 0.0f);
    EXPECT_FLOAT_EQ(copy->gravityScale, -0.5f);
    EXPECT_FLOAT_EQ(copy->drag, 0.3f);
    EXPECT_TRUE(copy->collideWithGround);
    EXPECT_FLOAT_EQ(copy->groundHeight, -2.0f);
    EXPECT_FLOAT_EQ(copy->bounce, 0.8f);
    EXPECT_EQ(copy->blendMode, Rendering::ParticleBlend::Additive);
    EXPECT_EQ(copy->texture, nullptr);

    EXPECT_FALSE(copy->IsPlaying());
    EXPECT_EQ(copy->GetAliveCount(), 0u);
    EXPECT_TRUE(copy->GetActiveBackendName().empty()) << "nothing runs until the system attaches in play mode";
}

TEST_F(ParticleSystemTest, AFieldMissingFromTheSavedJsonKeepsItsDefault)
{
    ParticleSystem *system = Make();
    system->maxParticles = 77;
    system->Deserialize(json{{"emissionRate", 7.0f}, {"backend", "CPU"}}, nullptr);
    EXPECT_FLOAT_EQ(system->emissionRate, 7.0f);
    EXPECT_EQ(system->backend, Particles::ParticleBackend::CPU);
    EXPECT_EQ(system->maxParticles, 77) << "not in the JSON: untouched";
    EXPECT_EQ(system->shape, Particles::EmitterShape::Cone);
}

// ---------------------------------------------------------------- the emitter's rules

TEST_F(ParticleSystemTest, EmitsAtTheRateWhilePlayingOnTheCpuBackend)
{
    const int cpuBefore = Particles::ParticleUsage::GetCount(false);
    ParticleSystem *system = Make();
    EXPECT_TRUE(system->GetActiveBackendName().empty()) << "not started before OnAttach";
    EXPECT_EQ(system->GetAliveCount(), 0u);

    system->OnAttach();
    EXPECT_EQ(system->GetActiveBackendName(), "CPU");
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore + 1);
    EXPECT_TRUE(system->IsPlaying()) << "playOnAwake";

    Step(system, 50); // one second at 50 a second
    EXPECT_NEAR(static_cast<double>(system->GetAliveCount()), 50.0, 1.0);

    system->OnDestroy();
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore);
    EXPECT_EQ(system->GetAliveCount(), 0u);
    EXPECT_TRUE(system->GetActiveBackendName().empty());
}

TEST_F(ParticleSystemTest, WithoutPlayOnAwakeNothingEmitsUntilPlay)
{
    ParticleSystem *system = Make();
    system->playOnAwake = false;
    system->OnAttach();
    EXPECT_FALSE(system->IsPlaying());
    Step(system, 25);
    EXPECT_EQ(system->GetAliveCount(), 0u);

    system->Play();
    EXPECT_TRUE(system->IsPlaying());
    Step(system, 25);
    EXPECT_NEAR(static_cast<double>(system->GetAliveCount()), 25.0, 1.0);
}

TEST_F(ParticleSystemTest, StopLetsParticlesFinishAndClearRemovesThem)
{
    ParticleSystem *system = Make();
    system->OnAttach();
    Step(system, 25);
    const std::size_t flying = system->GetAliveCount();
    ASSERT_GT(flying, 10u);

    system->Stop();
    EXPECT_FALSE(system->IsPlaying());
    Step(system, 25);
    EXPECT_EQ(system->GetAliveCount(), flying) << "no new ones, and the lifetime is 10 s";

    system->Clear();
    EXPECT_EQ(system->GetAliveCount(), 0u);
    Step(system, 5);
    EXPECT_EQ(system->GetAliveCount(), 0u) << "stopped stays stopped";
}

TEST_F(ParticleSystemTest, EmitBurstStartsParticlesNowWhetherOrNotPlaying)
{
    ParticleSystem *system = Make();
    system->playOnAwake = false;
    system->OnAttach();
    system->EmitBurst(7);
    EXPECT_EQ(system->GetAliveCount(), 7u);
    system->EmitBurst(0);
    system->EmitBurst(-5);
    EXPECT_EQ(system->GetAliveCount(), 7u) << "zero or negative asks for nothing";
    EXPECT_FALSE(system->IsPlaying()) << "a burst doesn't start the stream";
}

TEST_F(ParticleSystemTest, EveryPlayStartsTheBurstCount)
{
    ParticleSystem *system = Make();
    system->playOnAwake = false;
    system->burstCount = 5;
    system->emissionRate = 0.0f;
    system->OnAttach();
    system->Play();
    EXPECT_EQ(system->GetAliveCount(), 5u);
    system->Play();
    EXPECT_EQ(system->GetAliveCount(), 10u);
}

TEST_F(ParticleSystemTest, ABurstAskedForBeforeTheSystemStartsIsEmittedWhenItDoes)
{
    ParticleSystem *system = Make();
    system->playOnAwake = false;
    system->EmitBurst(9);
    EXPECT_EQ(system->GetAliveCount(), 0u);
    system->OnAttach();
    EXPECT_EQ(system->GetAliveCount(), 9u);

    // A Play before the start does not double its burst when playOnAwake is on as well
    ParticleSystem *both = Make("Both");
    both->burstCount = 4;
    both->emissionRate = 0.0f;
    both->Play();
    both->OnAttach();
    EXPECT_EQ(both->GetAliveCount(), 4u);
}

TEST_F(ParticleSystemTest, ParticlesStartAtTheObjectsPosition)
{
    ParticleSystem *system = Make();
    system->playOnAwake = false;
    system->GetGameObject().GetPositionable()->SetPosition(Math::Vector3(3.0f, 4.0f, 5.0f));
    system->OnAttach();
    system->EmitBurst(3);
    std::vector<Math::Vector4> particles;
    system->GetParticles(particles);
    ASSERT_EQ(particles.size(), 3u);
    for (const Math::Vector4 &p : particles)
    {
        EXPECT_NEAR(p.x, 3.0f, 1e-4f);
        EXPECT_NEAR(p.y, 4.0f, 1e-4f);
        EXPECT_NEAR(p.z, 5.0f, 1e-4f);
        EXPECT_FLOAT_EQ(p.w, 0.0f);
    }
}

TEST_F(ParticleSystemTest, TheSameSeedGivesTheSameParticlesAndAnotherSeedDoesNot)
{
    const auto run = [&](const int seed)
    {
        ParticleSystem *system = Make("Seeded" + std::to_string(seed));
        system->seed = seed;
        system->shapeRadius = 0.5f;
        system->coneAngle = 45.0f;
        system->lifetimeMin = 0.4f;
        system->lifetimeMax = 1.2f;
        system->OnAttach();
        Step(system, 80);
        std::vector<Math::Vector4> particles;
        system->GetParticles(particles);
        return particles;
    };
    const std::vector<Math::Vector4> a = run(5);
    const std::vector<Math::Vector4> b = run(5);
    const std::vector<Math::Vector4> c = run(6);
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        EXPECT_EQ(a[i].x, b[i].x);
        EXPECT_EQ(a[i].y, b[i].y);
        EXPECT_EQ(a[i].z, b[i].z);
        EXPECT_EQ(a[i].w, b[i].w);
    }
    bool differs = a.size() != c.size();
    for (std::size_t i = 0; !differs && i < a.size(); ++i)
    {
        differs = a[i].x != c[i].x || a[i].y != c[i].y || a[i].z != c[i].z;
    }
    EXPECT_TRUE(differs);
}

// ---------------------------------------------------------------- the cap

TEST_F(ParticleSystemTest, MaxParticlesCapsTheSystemWithOneWarning)
{
    WarningCapture warnings;
    ParticleSystem *system = Make("Capped");
    system->playOnAwake = false;
    system->maxParticles = 10;
    system->OnAttach();

    system->EmitBurst(25);
    EXPECT_EQ(system->GetAliveCount(), 10u);
    EXPECT_EQ(warnings.Count("reached maxParticles"), 1);
    EXPECT_EQ(warnings.Count("Capped"), 1) << "the warning names the object";

    system->EmitBurst(5);
    EXPECT_EQ(system->GetAliveCount(), 10u);
    EXPECT_EQ(warnings.Count("reached maxParticles"), 1) << "one warning, not one per dropped particle";

    // The stream drops its particles too, still without repeating the warning
    system->Play();
    Step(system, 20);
    EXPECT_EQ(system->GetAliveCount(), 10u);
    EXPECT_EQ(warnings.Count("reached maxParticles"), 1);
}

TEST_F(ParticleSystemTest, ParticlesExpiringMakeRoomAgain)
{
    ParticleSystem *system = Make();
    system->playOnAwake = false;
    system->maxParticles = 20;
    system->lifetimeMin = 0.1f;
    system->lifetimeMax = 0.1f;
    system->OnAttach();
    system->EmitBurst(20);
    ASSERT_EQ(system->GetAliveCount(), 20u);
    Step(system, 8); // past the lifetime
    EXPECT_EQ(system->GetAliveCount(), 0u);
    system->EmitBurst(20);
    EXPECT_EQ(system->GetAliveCount(), 20u);
}

TEST_F(ParticleSystemTest, AMaxParticlesAboveTheLimitIsLoweredWithAWarning)
{
    WarningCapture warnings;
    ParticleSystem *system = Make("Huge");
    system->playOnAwake = false;
    system->maxParticles = ParticleSystem::kMaxParticlesLimit + 1000;
    system->OnAttach();
    EXPECT_EQ(warnings.Count("above the limit"), 1);
    system->EmitBurst(ParticleSystem::kMaxParticlesLimit);
    EXPECT_EQ(system->GetAliveCount(), static_cast<std::size_t>(ParticleSystem::kMaxParticlesLimit));
    system->EmitBurst(10);
    EXPECT_EQ(system->GetAliveCount(), static_cast<std::size_t>(ParticleSystem::kMaxParticlesLimit))
        << "the limit, not the larger setting";
}

TEST_F(ParticleSystemTest, AMaxParticlesOfZeroEmitsNothing)
{
    WarningCapture warnings;
    ParticleSystem *system = Make();
    system->maxParticles = 0;
    system->OnAttach();
    Step(system, 25);
    EXPECT_EQ(system->GetAliveCount(), 0u);
}

// ---------------------------------------------------------------- backend selection

TEST_F(ParticleSystemTest, AutoUsesTheCpuBackendQuietlyWithoutAGpuBackend)
{
    WarningCapture warnings;
    ParticleSystem *system = Make();
    system->backend = Particles::ParticleBackend::Auto;
    system->OnAttach();
    EXPECT_EQ(system->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("PhysX"), 0);
}

TEST_F(ParticleSystemTest, PhysXWithoutAGpuFallsBackToTheCpuWithOneWarning)
{
    WarningCapture warnings;
    Application::GetInstance().Set3DPhysicsBackend(std::make_unique<TestSupport::FakePhysicsBackend>(
        false, Physics::GpuStatus{Physics::GpuState::Unavailable, "no CUDA device"}));

    ParticleSystem *system = Make("NeedsGpu");
    system->backend = Particles::ParticleBackend::PhysX;
    system->OnAttach();
    EXPECT_EQ(system->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1);
    EXPECT_EQ(warnings.Count("no CUDA device"), 1) << "the reason from the GPU probe is in it";
    EXPECT_EQ(warnings.Count("NeedsGpu"), 1);

    // It still simulates
    Step(system, 25);
    EXPECT_GT(system->GetAliveCount(), 0u);
    Step(system, 25);
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1) << "once, not every step";
}

TEST_F(ParticleSystemTest, PhysXWithNoPhysicsBackendAtAllAlsoFallsBack)
{
    WarningCapture warnings;
    ParticleSystem *system = Make();
    system->backend = Particles::ParticleBackend::PhysX;
    system->OnAttach();
    EXPECT_EQ(system->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1);
}

TEST_F(ParticleSystemTest, CpuIsNeverWarnedAbout)
{
    WarningCapture warnings;
    ParticleSystem *system = Make();
    system->backend = Particles::ParticleBackend::CPU;
    system->OnAttach();
    EXPECT_EQ(system->GetActiveBackendName(), "CPU");
    EXPECT_TRUE(warnings.warnings.empty());
}

TEST_F(ParticleSystemTest, AutoAndPhysXUseTheGpuBackendTheSceneOffersAndCpuStillWins)
{
    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(true);
    TestSupport::FakePhysicsBackend *physics = fake.get();
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    WarningCapture warnings;

    const int gpuBefore = Particles::ParticleUsage::GetCount(true);
    ParticleSystem *autoSystem = Make("Auto");
    autoSystem->backend = Particles::ParticleBackend::Auto;
    autoSystem->OnAttach();
    ParticleSystem *physxSystem = Make("PhysX");
    physxSystem->backend = Particles::ParticleBackend::PhysX;
    physxSystem->OnAttach();
    ParticleSystem *cpuSystem = Make("Cpu");
    cpuSystem->backend = Particles::ParticleBackend::CPU;
    cpuSystem->OnAttach();

    EXPECT_EQ(autoSystem->GetActiveBackendName(), "PhysX");
    EXPECT_EQ(physxSystem->GetActiveBackendName(), "PhysX");
    EXPECT_EQ(cpuSystem->GetActiveBackendName(), "CPU");
    EXPECT_EQ(physics->gpu.created, 2);
    EXPECT_EQ(Particles::ParticleUsage::GetCount(true), gpuBefore + 2);
    EXPECT_TRUE(warnings.warnings.empty());

    // The simulation really goes through the seam
    Step(autoSystem, 50);
    EXPECT_NEAR(static_cast<double>(autoSystem->GetAliveCount()), 50.0, 1.0);

    autoSystem->OnDestroy();
    physxSystem->OnDestroy();
    EXPECT_EQ(physics->gpu.destroyed, 2);
    EXPECT_EQ(Particles::ParticleUsage::GetCount(true), gpuBefore);
}

TEST_F(ParticleSystemTest, ASystemOutlivingItsPhysicsBackendIsInertNotACrash)
{
    Application::GetInstance().Set3DPhysicsBackend(std::make_unique<TestSupport::FakePhysicsBackend>(true));
    ParticleSystem *system = Make();
    system->OnAttach();
    Step(system, 5);
    ASSERT_GT(system->GetAliveCount(), 0u);

    Application::GetInstance().Set3DPhysicsBackend(nullptr); // the GPU backend is gone
    EXPECT_NO_THROW(Step(system, 5));
    EXPECT_EQ(system->GetAliveCount(), 0u);
    std::vector<Math::Vector4> particles(3);
    system->GetParticles(particles);
    EXPECT_TRUE(particles.empty());
    system->EmitBurst(5);
    system->Clear();
    EXPECT_NO_THROW(system->OnDestroy());
}

TEST_F(ParticleSystemTest, AReplacedPhysicsBackendMakesAPhysXSystemRestartOnTheCpuWithOneWarning)
{
    Application::GetInstance().Set3DPhysicsBackend(std::make_unique<TestSupport::FakePhysicsBackend>(true));
    const int gpuBefore = Particles::ParticleUsage::GetCount(true);
    const int cpuBefore = Particles::ParticleUsage::GetCount(false);

    ParticleSystem *system = Make();
    system->OnAttach();
    ASSERT_EQ(system->GetActiveBackendName(), "PhysX");
    Step(system, 5);
    ASSERT_GT(system->GetAliveCount(), 0u);
    EXPECT_EQ(Particles::ParticleUsage::GetCount(true), gpuBefore + 1);

    // The replacement has a particle backend of its own; the old handle must not reach it
    auto second = std::make_unique<TestSupport::FakePhysicsBackend>(true);
    TestSupport::FakePhysicsBackend *secondPtr = second.get();
    Application::GetInstance().Set3DPhysicsBackend(std::move(second));

    WarningCapture warnings;
    Step(system, 5);

    EXPECT_EQ(system->GetActiveBackendName(), "CPU");
    EXPECT_EQ(secondPtr->gpu.created, 0);
    EXPECT_EQ(secondPtr->gpu.destroyed, 0);
    EXPECT_EQ(Particles::ParticleUsage::GetCount(true), gpuBefore);
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore + 1);
    EXPECT_GT(system->GetAliveCount(), 0u);
    EXPECT_EQ(warnings.Count("physics backend changed"), 1);

    system->OnDestroy();
    EXPECT_EQ(secondPtr->gpu.destroyed, 0);
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore);
}

// ---------------------------------------------------------------- physics and pause

TEST_F(ParticleSystemTest, GravityIsTheWorldGravityTimesTheScale)
{
    ParticleSystem *system = Make();
    system->gravityScale = 0.5f;
    EXPECT_NEAR(system->BuildParams().gravity.y, -9.81f * 0.5f, 1e-5f) << "no physics backend: the default gravity";

    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(false);
    fake->gravity = Math::Vector3(0.0f, -20.0f, 4.0f);
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    EXPECT_NEAR(system->BuildParams().gravity.y, -10.0f, 1e-5f);
    EXPECT_NEAR(system->BuildParams().gravity.z, 2.0f, 1e-5f);

    system->gravityScale = 0.0f;
    EXPECT_FLOAT_EQ(system->BuildParams().gravity.y, 0.0f);
}

TEST_F(ParticleSystemTest, TheLifetimeRangeIsNeverInverted)
{
    ParticleSystem *system = Make();
    system->lifetimeMin = 3.0f;
    system->lifetimeMax = 1.0f;
    const Physics::ParticleParams params = system->BuildParams();
    EXPECT_FLOAT_EQ(params.lifetimeMin, 3.0f);
    EXPECT_FLOAT_EQ(params.lifetimeMax, 3.0f);
    system->speedMin = 5.0f;
    system->speedMax = 2.0f;
    EXPECT_FLOAT_EQ(system->BuildShapeSettings().speedMax, 5.0f);
}

TEST_F(ParticleSystemTest, PausingFreezesEmissionAndMotion)
{
    ParticleSystem *system = Make();
    system->OnAttach();
    Step(system, 25);
    std::vector<Math::Vector4> before;
    system->GetParticles(before);
    ASSERT_FALSE(before.empty());

    Time::SetTimeScale(0.0f); // paused: the fixed step is 0
    Step(system, 50);
    std::vector<Math::Vector4> during;
    system->GetParticles(during);
    ASSERT_EQ(during.size(), before.size()) << "nothing emitted while paused";
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_EQ(during[i].x, before[i].x);
        EXPECT_EQ(during[i].y, before[i].y);
        EXPECT_EQ(during[i].z, before[i].z);
        EXPECT_EQ(during[i].w, before[i].w);
    }

    Time::SetTimeScale(1.0f);
    Step(system, 10);
    EXPECT_GT(system->GetAliveCount(), before.size());
}

TEST_F(ParticleSystemTest, DestroyingTheComponentReleasesItsBackendSystem)
{
    const int cpuBefore = Particles::ParticleUsage::GetCount(false);
    {
        auto object = GameObject::Create("Temp");
        auto *system = object->AddComponent<ParticleSystem>();
        system->OnAttach();
        EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore + 1);
        // object (and the component) go out of scope without OnDestroy, as an object that never joined a scene
    }
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore);
}

TEST_F(ParticleSystemTest, OnAttachTwiceMakesOneSystem)
{
    const int cpuBefore = Particles::ParticleUsage::GetCount(false);
    ParticleSystem *system = Make();
    system->OnAttach();
    system->OnAttach();
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), cpuBefore + 1);
}

// ---------------------------------------------------------------- health detail

TEST(ParticleHealthDetailTest, ComposeLeavesTheGpuProbesDetailAloneWithoutSystemsAndAppendsTheBackendsWithThem)
{
    const std::string probe = "physics.gpu is off";
    if (Particles::ParticleUsage::GetCount(false) == 0 && Particles::ParticleUsage::GetCount(true) == 0)
    {
        EXPECT_EQ(Particles::ParticleUsage::Compose(probe), probe) << "exactly what the probe said";
        EXPECT_EQ(Particles::ParticleUsage::Compose(""), "");
    }

    Particles::ParticleUsage::Add(false);
    const std::string withSystem = Particles::ParticleUsage::Compose(probe);
    EXPECT_EQ(withSystem.rfind(probe + "; ParticleSystems:", 0), 0u) << withSystem;
    EXPECT_NE(withSystem.find("CPU"), std::string::npos) << withSystem;
    const std::string noProbe = Particles::ParticleUsage::Compose("");
    EXPECT_EQ(noProbe.rfind("ParticleSystems:", 0), 0u) << noProbe;
    Particles::ParticleUsage::Remove(false);
}
