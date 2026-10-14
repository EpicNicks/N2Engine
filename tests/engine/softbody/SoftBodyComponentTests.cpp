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
#include <nlohmann/json.hpp>

#include "../particles/FakePhysicsBackend.hpp"
#include "SoftBodyTestSupport.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SoftBody.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

// The SoftBody component without a renderer or a scene: registration and reflection, save and load, the inert cases
// (one warning each), backend selection with a fake physics backend, the runtime-only mesh override on the sibling
// MeshRenderer, Reset and AddVelocity, and the freeze while paused. The fixed step is driven by calling
// OnFixedUpdate, as Scene::FixedUpdate does; OnAttach stands for entering play mode (a scene opened for editing
// never attaches its components).

using namespace N2Engine;
using json = nlohmann::json;
using Physics::SoftBody;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using TestSupport::SoftBodyWarningCapture;

namespace
{
    struct Rig
    {
        GameObject::Ptr object;
        SoftBody *body = nullptr;
        MeshRenderer *renderer = nullptr;
        std::shared_ptr<Mesh> mesh;
    };

    class SoftBodyComponentTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
            SoftBodies::TetCache::Instance().Clear();
        }

        void TearDown() override
        {
            _rigs.clear();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            SoftBodies::TetCache::Instance().Clear();
        }

        /// A new object (never in a scene) with a MeshRenderer on a unit box and a SoftBody: quiet, stiff settings
        Rig &Make(const std::string &name = "Jelly", const bool withRenderer = true)
        {
            Rig rig;
            rig.object = GameObject::Create(name);
            if (withRenderer)
            {
                rig.mesh = TestSupport::MakeBoxMesh();
                rig.renderer = rig.object->AddComponent<MeshRenderer>();
                rig.renderer->SetMesh(rig.mesh);
            }
            rig.body = rig.object->AddComponent<SoftBody>();
            rig.body->resolution = 4;
            rig.body->stiffness = 1.0e6f;
            _rigs.push_back(std::move(rig));
            return _rigs.back();
        }

        static void Step(SoftBody *body, const int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                body->OnFixedUpdate();
            }
        }

        static Math::Vector3 MeanPosition(const Mesh &mesh)
        {
            double x = 0.0, y = 0.0, z = 0.0;
            for (const auto &vertex : mesh.GetVertices())
            {
                x += vertex.position[0];
                y += vertex.position[1];
                z += vertex.position[2];
            }
            const double n = static_cast<double>(mesh.GetVertices().size());
            return Math::Vector3(static_cast<float>(x / n), static_cast<float>(y / n), static_cast<float>(z / n));
        }

        std::vector<Rig> _rigs;
    };

    /// Saves one root object in a scene and loads the scene back, as SceneManager does
    GameObject::Ptr RoundTrip(const GameObject::Ptr &go, std::unique_ptr<Scene> &loadedScene)
    {
        const auto scene = Scene::Create("SoftBodyRoundTrip_" + go->GetName());
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

TEST_F(SoftBodyComponentTest, IsRegisteredAndDescribesItsFields)
{
    auto &registry = ComponentRegistry::Instance();
    ASSERT_TRUE(registry.IsRegistered("SoftBody"));
    EXPECT_FALSE(registry.IsSingleton("SoftBody"));

    const std::optional<ComponentSchema> schema = registry.Describe("SoftBody");
    ASSERT_TRUE(schema.has_value());
    const auto find = [&](const std::string &name) -> const FieldInfo *
    {
        const auto it = std::ranges::find(schema->fields, name, &FieldInfo::name);
        return it == schema->fields.end() ? nullptr : &*it;
    };

    const FieldInfo *backend = find("backend");
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->kind, FieldKind::Enum);
    EXPECT_EQ(backend->enumOptions, (std::vector<std::string>{"Auto", "PhysX", "CPU"}));
    EXPECT_EQ(schema->defaults.at("backend"), "Auto");

    const FieldInfo *model = find("materialModel");
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->kind, FieldKind::Enum);
    EXPECT_EQ(model->enumOptions, (std::vector<std::string>{"CoRotational", "NeoHookean"}));
    EXPECT_EQ(schema->defaults.at("materialModel"), "CoRotational");

    for (const char *intField : {"resolution", "solverIterations"})
    {
        ASSERT_NE(find(intField), nullptr) << intField;
        EXPECT_EQ(find(intField)->kind, FieldKind::Int) << intField;
    }
    for (const char *floatField : {"density", "stiffness", "poisson", "damping", "friction", "groundHeight"})
    {
        ASSERT_NE(find(floatField), nullptr) << floatField;
        EXPECT_EQ(find(floatField)->kind, FieldKind::Float) << floatField;
    }
    for (const char *boolField : {"collideWithGround", "selfCollision"})
    {
        ASSERT_NE(find(boolField), nullptr) << boolField;
        EXPECT_EQ(find(boolField)->kind, FieldKind::Bool) << boolField;
    }
    EXPECT_EQ(schema->defaults.at("resolution"), 10);
    EXPECT_FALSE(schema->defaults.at("collideWithGround").get<bool>());

    // Exactly the documented fields (apart from the base ones): no pin settings, no asset field
    std::vector<std::string> names;
    for (const FieldInfo &field : schema->fields)
    {
        names.push_back(field.name);
    }
    std::ranges::sort(names);
    std::vector<std::string> expected{"backend",   "collideWithGround", "damping",          "density",
                                      "friction",  "groundHeight",      "materialModel",    "poisson",
                                      "resolution", "selfCollision",    "solverIterations", "stiffness"};
    std::ranges::sort(expected);
    EXPECT_EQ(names, expected);

    // Ranges keep the inspector's values inside what the backends accept
    ASSERT_TRUE(find("resolution")->range.has_value());
    EXPECT_EQ(find("resolution")->range->first, 2.0);
    EXPECT_EQ(find("resolution")->range->second, 32.0);
    ASSERT_TRUE(find("poisson")->range.has_value());
    EXPECT_EQ(find("poisson")->range->second, 0.49);
}

TEST_F(SoftBodyComponentTest, SavesItsSettingsAndNoSimulationState)
{
    Rig &rig = Make();
    rig.body->backend = SoftBodies::SoftBodyBackend::CPU;
    const json beforePlay = rig.body->Serialize();
    const json rendererBefore = rig.renderer->Serialize();
    EXPECT_EQ(beforePlay.at("backend"), "CPU");

    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "CPU");
    Step(rig.body, 10);

    EXPECT_EQ(rig.body->Serialize(), beforePlay) << "a running body saves what it saved before play";
    for (const char *runtimeKey : {"handle", "nodes", "positions", "tets", "runtimeMesh", "deformedMesh", "deformed"})
    {
        EXPECT_FALSE(rig.body->Serialize().contains(runtimeKey)) << runtimeKey << " is runtime state, never saved";
    }

    // The MeshRenderer is saved exactly as before: the override is not part of it
    const json rendererDuring = rig.renderer->Serialize();
    EXPECT_EQ(rendererDuring, rendererBefore);
    for (const char *runtimeKey : {"deformed", "_deformed", "deformedMesh", "runtimeMesh"})
    {
        EXPECT_FALSE(rendererDuring.contains(runtimeKey)) << runtimeKey;
    }
}

TEST_F(SoftBodyComponentTest, EverySettingSurvivesASceneLoadAndTheLoadedBodyIsNotStarted)
{
    auto go = GameObject::Create("Pudding");
    auto *body = go->AddComponent<SoftBody>();
    body->backend = SoftBodies::SoftBodyBackend::PhysX;
    body->resolution = 7;
    body->density = 321.5f;
    body->stiffness = 2.5e5f;
    body->poisson = 0.41f;
    body->damping = 3.5f;
    body->friction = 0.125f;
    body->solverIterations = 6;
    body->collideWithGround = true;
    body->groundHeight = -2.25f;
    body->materialModel = Physics::SoftMaterialModel::NeoHookean;
    body->selfCollision = true;

    std::unique_ptr<Scene> scene;
    const auto loaded = RoundTrip(go, scene);
    ASSERT_NE(loaded, nullptr);
    const auto *copy = loaded->GetComponent<SoftBody>();
    ASSERT_NE(copy, nullptr) << "SoftBody wasn't registered, so loading dropped it";

    EXPECT_EQ(copy->backend, SoftBodies::SoftBodyBackend::PhysX);
    EXPECT_EQ(copy->resolution, 7);
    EXPECT_FLOAT_EQ(copy->density, 321.5f);
    EXPECT_FLOAT_EQ(copy->stiffness, 2.5e5f);
    EXPECT_FLOAT_EQ(copy->poisson, 0.41f);
    EXPECT_FLOAT_EQ(copy->damping, 3.5f);
    EXPECT_FLOAT_EQ(copy->friction, 0.125f);
    EXPECT_EQ(copy->solverIterations, 6);
    EXPECT_TRUE(copy->collideWithGround);
    EXPECT_FLOAT_EQ(copy->groundHeight, -2.25f);
    EXPECT_EQ(copy->materialModel, Physics::SoftMaterialModel::NeoHookean);
    EXPECT_TRUE(copy->selfCollision);

    EXPECT_TRUE(copy->GetActiveBackendName().empty()) << "nothing runs until the body attaches in play mode";
    EXPECT_EQ(copy->GetRuntimeMesh(), nullptr);
    EXPECT_EQ(copy->GetTetMesh(), nullptr);
}

TEST_F(SoftBodyComponentTest, AFieldMissingFromTheSavedJsonKeepsItsDefault)
{
    Rig &rig = Make("Defaults", false);
    rig.body->resolution = 9;
    rig.body->Deserialize(json{{"stiffness", 7.0e4f}, {"backend", "CPU"}}, nullptr);
    EXPECT_FLOAT_EQ(rig.body->stiffness, 7.0e4f);
    EXPECT_EQ(rig.body->backend, SoftBodies::SoftBodyBackend::CPU);
    EXPECT_EQ(rig.body->resolution, 9) << "not in the JSON: untouched";
    EXPECT_EQ(rig.body->materialModel, Physics::SoftMaterialModel::CoRotational);
}

TEST_F(SoftBodyComponentTest, TheDefaultsAreTheDocumentedOnes)
{
    const auto object = GameObject::Create("Fresh");
    const auto *fresh = object->AddComponent<SoftBody>();
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->backend, SoftBodies::SoftBodyBackend::Auto);
    EXPECT_EQ(fresh->resolution, 10);
    EXPECT_FLOAT_EQ(fresh->density, 1000.0f);
    EXPECT_FLOAT_EQ(fresh->stiffness, 1.0e5f);
    EXPECT_FLOAT_EQ(fresh->poisson, 0.3f);
    EXPECT_FLOAT_EQ(fresh->damping, 0.1f);
    EXPECT_FLOAT_EQ(fresh->friction, 0.5f);
    EXPECT_EQ(fresh->solverIterations, 2);
    EXPECT_FALSE(fresh->collideWithGround);
    EXPECT_FLOAT_EQ(fresh->groundHeight, 0.0f);
    EXPECT_FALSE(fresh->selfCollision);
    EXPECT_EQ(fresh->materialModel, Physics::SoftMaterialModel::CoRotational);
}

// ---------------------------------------------------------------- starting, and the inert cases

TEST_F(SoftBodyComponentTest, StartsOnTheCpuBackendAndDrawsADeformedCopyOfTheMesh)
{
    SoftBodyWarningCapture warnings;
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make();
    const std::uint64_t sourceVersion = rig.mesh->GetVersion();
    const Math::Vector3 sourceMean = MeanPosition(*rig.mesh);

    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_TRUE(warnings.warnings.empty());
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    ASSERT_NE(rig.body->GetTetMesh(), nullptr);
    EXPECT_EQ(rig.body->GetTetMesh()->resolution, 4);

    // The renderer draws the runtime copy; its own (saved) mesh is untouched
    const std::shared_ptr<Mesh> runtime = rig.body->GetRuntimeMesh();
    ASSERT_NE(runtime, nullptr);
    EXPECT_NE(runtime, rig.mesh);
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), runtime);
    EXPECT_EQ(rig.renderer->GetDrawnMesh(), runtime);
    EXPECT_EQ(rig.renderer->GetMesh(), rig.mesh);
    EXPECT_EQ(runtime->GetVertices().size(), rig.mesh->GetVertices().size());
    EXPECT_EQ(runtime->GetIndices(), rig.mesh->GetIndices());
    EXPECT_EQ(runtime->GetSubmeshCount(), rig.mesh->GetSubmeshCount());

    // It falls: each fixed step is a new version of the runtime mesh, and the vertices move
    const std::uint64_t runtimeVersion = runtime->GetVersion();
    Step(rig.body, 10);
    EXPECT_GE(runtime->GetVersion(), runtimeVersion + 10);
    const Math::Vector3 mean = MeanPosition(*runtime);
    EXPECT_LT(mean.y, sourceMean.y - 0.05f);
    EXPECT_NEAR(mean.x, sourceMean.x, 1e-3f);
    EXPECT_NEAR(mean.z, sourceMean.z, 1e-3f);

    // The source mesh never changed
    EXPECT_EQ(rig.mesh->GetVersion(), sourceVersion);
    const Math::Vector3 stillMean = MeanPosition(*rig.mesh);
    EXPECT_EQ(stillMean.y, sourceMean.y);

    // What isn't a position is the rest data: texture coordinates, colours and tangents; normals are recomputed unit
    for (std::size_t i = 0; i < runtime->GetVertices().size(); ++i)
    {
        const auto &deformed = runtime->GetVertices()[i];
        const auto &rest = rig.mesh->GetVertices()[i];
        EXPECT_EQ(deformed.texCoord[0], rest.texCoord[0]);
        EXPECT_EQ(deformed.texCoord[1], rest.texCoord[1]);
        EXPECT_EQ(deformed.color[0], rest.color[0]);
        EXPECT_EQ(deformed.tangent[3], rest.tangent[3]);
        const float length = std::sqrt(deformed.normal[0] * deformed.normal[0] + deformed.normal[1] * deformed.normal[1] +
                                       deformed.normal[2] * deformed.normal[2]);
        EXPECT_NEAR(length, 1.0f, 1e-3f);
    }

    rig.body->OnDestroy();
}

TEST_F(SoftBodyComponentTest, TheNormalsOfAFlatFaceAreItsAreaWeightedNormal)
{
    // The test box's vertices all start with the normal (0, 1, 0); once the body is running the normals are
    // recomputed from the triangles, so the vertices of the top face (y = +0.5) get an outward normal with a
    // positive y and the bottom face's a negative one (at rest the box is still a box)
    Rig &rig = Make("Normals");
    rig.body->OnAttach();
    rig.body->OnFixedUpdate();
    const auto &vertices = rig.body->GetRuntimeMesh()->GetVertices();
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        const auto &rest = rig.mesh->GetVertices()[i];
        // The mean of the three face normals meeting at a corner points away from the box's centre
        const float outward = (rest.position[0] > 0 ? 1.0f : -1.0f) * vertices[i].normal[0] +
                              (rest.position[1] > 0 ? 1.0f : -1.0f) * vertices[i].normal[1] +
                              (rest.position[2] > 0 ? 1.0f : -1.0f) * vertices[i].normal[2];
        EXPECT_GT(outward, 0.5f) << "vertex " << i;
    }
}

TEST_F(SoftBodyComponentTest, EditModeLeavesTheMeshRendererAlone)
{
    // A scene opened for editing never attaches its components: nothing but the fields exist
    Rig &rig = Make();
    const std::uint64_t version = rig.mesh->GetVersion();
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(rig.body->GetRuntimeMesh(), nullptr);
    EXPECT_TRUE(rig.body->GetActiveBackendName().empty());
    Step(rig.body, 10); // a fixed step that somehow ran does nothing without a body
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(rig.renderer->GetDrawnMesh(), rig.mesh);
    EXPECT_EQ(rig.renderer->GetMesh(), rig.mesh);
    EXPECT_EQ(rig.mesh->GetVersion(), version);
}

TEST_F(SoftBodyComponentTest, WithoutAMeshRendererItIsInertWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make("Bare", false);
    rig.body->OnAttach();
    EXPECT_TRUE(rig.body->GetActiveBackendName().empty());
    EXPECT_EQ(warnings.Count("needs a MeshRenderer"), 1);
    EXPECT_EQ(warnings.Count("Bare"), 1) << "the warning names the object";
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);

    Step(rig.body, 10);
    EXPECT_EQ(warnings.warnings.size(), 1u) << "once, not every step";
    EXPECT_NO_THROW(rig.body->AddVelocity(Math::Vector3(1.0f, 0.0f, 0.0f)));
    EXPECT_NO_THROW(rig.body->Reset());
    EXPECT_NO_THROW(rig.body->OnDestroy());
}

TEST_F(SoftBodyComponentTest, WithARigidbodyOnTheSameObjectItIsInertWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make("Both");
    rig.object->AddComponent<Physics::Rigidbody>();
    rig.body->OnAttach();
    EXPECT_TRUE(rig.body->GetActiveBackendName().empty());
    EXPECT_EQ(warnings.Count("Rigidbody"), 1);
    EXPECT_EQ(warnings.Count("Both"), 1);
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr) << "inert: the mesh is drawn as it is";
    Step(rig.body, 5);
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(warnings.warnings.size(), 1u);
}

TEST_F(SoftBodyComponentTest, AMeshRendererWithoutAMeshMakesItInertWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make("Empty");
    rig.renderer->SetMesh(nullptr);
    rig.body->OnAttach();
    EXPECT_TRUE(rig.body->GetActiveBackendName().empty());
    EXPECT_EQ(warnings.Count("has no mesh"), 1);
}

TEST_F(SoftBodyComponentTest, AMeshThatCannotBeTetrahedralisedMakesItInertWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make("Flat");
    rig.renderer->SetMesh(Mesh::Create(Mesh::MakeQuad())); // a quad has no volume
    rig.body->OnAttach();
    EXPECT_TRUE(rig.body->GetActiveBackendName().empty());
    EXPECT_EQ(warnings.Count("can't make a tetrahedral body"), 1);
    EXPECT_EQ(warnings.Count("flat"), 1) << "it says why";
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(rig.body->GetTetMesh(), nullptr);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);
    Step(rig.body, 5);
    EXPECT_EQ(warnings.warnings.size(), 1u);
}

TEST_F(SoftBodyComponentTest, OnAttachTwiceMakesOneBody)
{
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make();
    rig.body->OnAttach();
    const std::shared_ptr<Mesh> runtime = rig.body->GetRuntimeMesh();
    rig.body->OnAttach();
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    EXPECT_EQ(rig.body->GetRuntimeMesh(), runtime);
}

// ---------------------------------------------------------------- the resolution caps

TEST_F(SoftBodyComponentTest, AResolutionOutsideTheRangeIsClampedWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make("Huge");
    rig.body->resolution = 1000;
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("outside"), 1);
    EXPECT_EQ(warnings.Count("Huge"), 2) << "the clamp warning and the tetrahedron cap warning name the object";
    ASSERT_NE(rig.body->GetTetMesh(), nullptr);
    EXPECT_LE(rig.body->GetTetMesh()->resolution, Physics::kMaxTetResolution);
    EXPECT_LE(rig.body->GetTetMesh()->tets.size(), Physics::kMaxTetsPerBody);

    Rig &small = Make("Tiny");
    small.body->resolution = -3;
    small.body->OnAttach();
    ASSERT_EQ(small.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("outside"), 2);
    EXPECT_EQ(small.body->GetTetMesh()->resolution, Physics::kMinTetResolution);
}

TEST_F(SoftBodyComponentTest, ABodyThatWouldHaveTooManyTetrahedraGetsALowerResolutionWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make("Dense");
    rig.body->resolution = Physics::kMaxTetResolution; // in range, but 163840 tetrahedra on a box
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("would need more than"), 1);
    EXPECT_EQ(warnings.Count("outside"), 0);
    EXPECT_LT(rig.body->GetTetMesh()->resolution, Physics::kMaxTetResolution);
    EXPECT_LE(rig.body->GetTetMesh()->tets.size(), Physics::kMaxTetsPerBody);
    Step(rig.body, 2); // and it runs
}

TEST_F(SoftBodyComponentTest, BodiesOnTheSameMeshShareOneCachedTetrahedralisation)
{
    SoftBodies::TetCache &cache = SoftBodies::TetCache::Instance();
    Rig &first = Make("First");
    const std::size_t misses = cache.GetMissCount();
    const std::size_t hits = cache.GetHitCount();
    first.body->OnAttach();
    EXPECT_EQ(cache.GetMissCount(), misses + 1);

    // Another object with the same mesh object and resolution
    Rig &second = Make("Second");
    second.renderer->SetMesh(first.mesh);
    second.body->OnAttach();
    EXPECT_EQ(cache.GetHitCount(), hits + 1);
    EXPECT_EQ(second.body->GetTetMesh(), first.body->GetTetMesh());

    // Playing again (a new body on the same mesh) is a hit too
    first.body->OnDestroy();
    Rig &third = Make("Third");
    third.renderer->SetMesh(first.mesh);
    third.body->OnAttach();
    EXPECT_EQ(cache.GetHitCount(), hits + 2);

    // The mesh changes: a miss
    const std::size_t missesBefore = cache.GetMissCount();
    ASSERT_TRUE(first.mesh->SetData(TestSupport::MakeWeldedBox(2.0f, 1.0f, 1.0f)));
    Rig &fourth = Make("Fourth");
    fourth.renderer->SetMesh(first.mesh);
    fourth.body->OnAttach();
    EXPECT_EQ(cache.GetMissCount(), missesBefore + 1);
    EXPECT_FALSE(first.mesh->HasCpuCache()) << "the picking structure's slot was never used";
}

// ---------------------------------------------------------------- the override is runtime-only and always cleared

TEST_F(SoftBodyComponentTest, DestroyingTheSoftBodyClearsTheOverrideAndReleasesTheBody)
{
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make();
    rig.body->OnAttach();
    ASSERT_NE(rig.renderer->GetDeformedMesh(), nullptr);
    ASSERT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);

    rig.body->OnDestroy();
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(rig.renderer->GetDrawnMesh(), rig.mesh) << "drawn as the mesh it was";
    EXPECT_EQ(rig.renderer->GetMesh(), rig.mesh);
    EXPECT_EQ(rig.body->GetRuntimeMesh(), nullptr);
    EXPECT_TRUE(rig.body->GetActiveBackendName().empty());
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);
    EXPECT_NO_THROW(Step(rig.body, 3));
    EXPECT_NO_THROW(rig.body->OnDestroy());
}

TEST_F(SoftBodyComponentTest, TheMeshRendererDropsTheOverrideWhenItIsDestroyed)
{
    Rig &rig = Make();
    rig.body->OnAttach();
    ASSERT_NE(rig.renderer->GetDeformedMesh(), nullptr);
    rig.renderer->OnDestroy();
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(rig.renderer->GetMesh(), rig.mesh);
    // The body carries on harmlessly, then goes
    EXPECT_NO_THROW(Step(rig.body, 3));
    EXPECT_NO_THROW(rig.body->OnDestroy());
}

TEST_F(SoftBodyComponentTest, RemovingTheSoftBodyFromItsObjectClearsTheOverride)
{
    Rig &rig = Make();
    rig.body->OnAttach();
    ASSERT_NE(rig.renderer->GetDeformedMesh(), nullptr);
    // RemoveComponent runs OnDestroy before it frees the component
    EXPECT_TRUE(rig.object->RemoveComponent<SoftBody>());
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    rig.body = nullptr;
}

TEST_F(SoftBodyComponentTest, AMeshRendererAloneNeverHasAnOverride)
{
    Rig &rig = Make("Plain", true);
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    rig.renderer->SetDeformedMesh(rig.mesh);
    EXPECT_EQ(rig.renderer->GetDrawnMesh(), rig.mesh);
    rig.renderer->ClearDeformedMesh();
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), nullptr);
    EXPECT_EQ(rig.renderer->GetDrawnMesh(), rig.mesh);
    // Without a mesh at all there is nothing drawn
    rig.renderer->SetMesh(nullptr);
    EXPECT_EQ(rig.renderer->GetDrawnMesh(), nullptr);
    EXPECT_FALSE(rig.renderer->GetBounds().has_value());
}

TEST_F(SoftBodyComponentTest, TheRendererBoundsFollowTheDeformedMeshAndTheSourceAfterwards)
{
    Rig &rig = Make();
    const auto restBounds = rig.renderer->GetBounds();
    ASSERT_TRUE(restBounds.has_value());
    rig.body->OnAttach();
    Step(rig.body, 25);
    const auto fallingBounds = rig.renderer->GetBounds();
    ASSERT_TRUE(fallingBounds.has_value());
    EXPECT_LT(fallingBounds->min.y, restBounds->min.y - 0.5f) << "the bounds are the deformed mesh's";
    rig.body->OnDestroy();
    const auto after = rig.renderer->GetBounds();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->min.y, restBounds->min.y);
}

// ---------------------------------------------------------------- simulation behaviour through the component

TEST_F(SoftBodyComponentTest, TheBodyRestsOnTheGroundPlane)
{
    Rig &rig = Make("Landing");
    rig.object->GetPositionable()->SetPosition(Math::Vector3(0.0f, 0.55f, 0.0f));
    rig.body->collideWithGround = true;
    rig.body->groundHeight = 0.0f;
    rig.body->OnAttach();
    Step(rig.body, 300);

    // The object is at y = 0.55, so a local y of -0.55 is the world ground: nothing below it
    for (const auto &vertex : rig.body->GetRuntimeMesh()->GetVertices())
    {
        EXPECT_GE(vertex.position[1] + 0.55f, -1e-3f);
    }
    // ... and the body is down on it, not floating
    const Math::Vector3 mean = MeanPosition(*rig.body->GetRuntimeMesh());
    EXPECT_LT(mean.y, 0.0f);
}

TEST_F(SoftBodyComponentTest, TheBodySimulatesInWorldSpaceSoMovingTheObjectDoesNotDragIt)
{
    Rig &rig = Make("Anchored");
    rig.body->damping = 0.0f;
    rig.body->OnAttach();
    rig.object->GetPositionable()->SetPosition(Math::Vector3(5.0f, 0.0f, 0.0f)); // in play, after the body started
    Step(rig.body, 2);
    // The body is still around the world origin, so in the moved object's local space it is 5 units to the left
    const Math::Vector3 mean = MeanPosition(*rig.body->GetRuntimeMesh());
    EXPECT_NEAR(mean.x, -5.0f, 0.05f);
}

TEST_F(SoftBodyComponentTest, AddVelocityPushesTheBody)
{
    Rig &pushed = Make("Pushed");
    Rig &still = Make("Still");
    pushed.body->OnAttach();
    still.body->OnAttach();
    pushed.body->AddVelocity(Math::Vector3(3.0f, 0.0f, 0.0f));
    Step(pushed.body, 10);
    Step(still.body, 10);
    const float pushedX = MeanPosition(*pushed.body->GetRuntimeMesh()).x;
    const float stillX = MeanPosition(*still.body->GetRuntimeMesh()).x;
    EXPECT_GT(pushedX, stillX + 0.4f);
}

TEST_F(SoftBodyComponentTest, ResetPutsTheBodyBackToItsRestShapeAtTheCurrentPose)
{
    SoftBodyWarningCapture warnings;
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make("Boing");
    EXPECT_NO_THROW(rig.body->Reset()) << "before the component attached: nothing to do";
    EXPECT_EQ(rig.body->GetRuntimeMesh(), nullptr);

    rig.body->OnAttach();
    Step(rig.body, 20);
    const float fallen = MeanPosition(*rig.body->GetRuntimeMesh()).y;
    ASSERT_LT(fallen, -0.5f);

    rig.object->GetPositionable()->SetPosition(Math::Vector3(2.0f, 0.0f, 0.0f));
    rig.body->Reset();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1) << "restarted, not duplicated";
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), rig.body->GetRuntimeMesh());

    // Back at the rest shape: stepping once moves it only a little from where it was put
    Step(rig.body, 1);
    const Math::Vector3 mean = MeanPosition(*rig.body->GetRuntimeMesh());
    EXPECT_NEAR(mean.y, 0.0f, 0.02f) << "at rest again, after one step";
    EXPECT_NEAR(mean.x, 0.0f, 0.02f) << "at the object's current pose: the object moved to x = 2 and so did the body";
    EXPECT_TRUE(warnings.warnings.empty());

    // And it falls again
    Step(rig.body, 20);
    EXPECT_LT(MeanPosition(*rig.body->GetRuntimeMesh()).y, -0.5f);
}

TEST_F(SoftBodyComponentTest, PausingFreezesTheBody)
{
    Rig &rig = Make();
    rig.body->OnAttach();
    Step(rig.body, 10);
    const std::shared_ptr<Mesh> runtime = rig.body->GetRuntimeMesh();
    const std::uint64_t version = runtime->GetVersion();
    const Math::Vector3 before = MeanPosition(*runtime);

    Time::SetTimeScale(0.0f); // paused: the fixed step is 0
    Step(rig.body, 50);
    EXPECT_EQ(runtime->GetVersion(), version) << "no step, no new mesh version";
    const Math::Vector3 during = MeanPosition(*runtime);
    EXPECT_EQ(during.y, before.y);

    Time::SetTimeScale(1.0f);
    Step(rig.body, 10);
    EXPECT_LT(MeanPosition(*runtime).y, before.y);
}

TEST_F(SoftBodyComponentTest, TheSettingsAreTheBackendsParametersAndGravityIsThePhysicsBackends)
{
    Rig &rig = Make();
    rig.body->density = 400.0f;
    rig.body->stiffness = 3.0e4f;
    rig.body->poisson = 0.2f;
    rig.body->damping = 0.7f;
    rig.body->friction = 0.9f;
    rig.body->solverIterations = 0; // below the minimum
    rig.body->collideWithGround = true;
    rig.body->groundHeight = -1.5f;

    Physics::SoftBodyParams params = rig.body->BuildParams();
    EXPECT_FLOAT_EQ(params.density, 400.0f);
    EXPECT_FLOAT_EQ(params.stiffness, 3.0e4f);
    EXPECT_FLOAT_EQ(params.poisson, 0.2f);
    EXPECT_FLOAT_EQ(params.damping, 0.7f);
    EXPECT_FLOAT_EQ(params.friction, 0.9f);
    EXPECT_EQ(params.solverIterations, 1u);
    EXPECT_TRUE(params.groundEnabled);
    EXPECT_FLOAT_EQ(params.groundHeight, -1.5f);
    EXPECT_FLOAT_EQ(params.gravity.y, -9.81f) << "no physics backend: the default";

    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(false);
    fake->gravity = Math::Vector3(0.0f, -1.62f, 0.0f);
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    params = rig.body->BuildParams();
    EXPECT_FLOAT_EQ(params.gravity.y, -1.62f) << "the physics backend's gravity";
}

TEST_F(SoftBodyComponentTest, ASettingChangedWhileRunningApplies)
{
    Rig &rig = Make();
    rig.body->OnAttach();
    Step(rig.body, 5);
    rig.body->collideWithGround = true;
    rig.body->groundHeight = 10.0f; // above the body: it is lifted onto the plane
    Step(rig.body, 1);
    const Math::Vector3 mean = MeanPosition(*rig.body->GetRuntimeMesh());
    EXPECT_GE(mean.y, 10.0f - 1e-3f);
}

// ---------------------------------------------------------------- backend selection

TEST_F(SoftBodyComponentTest, AutoUsesTheCpuBackendQuietlyWithoutAGpuBackend)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make();
    rig.body->backend = SoftBodies::SoftBodyBackend::Auto;
    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("PhysX"), 0);
    EXPECT_TRUE(warnings.warnings.empty());
}

TEST_F(SoftBodyComponentTest, PhysXWithoutAGpuFallsBackToTheCpuWithOneWarning)
{
    SoftBodyWarningCapture warnings;
    Application::GetInstance().Set3DPhysicsBackend(std::make_unique<TestSupport::FakePhysicsBackend>(
        false, Physics::GpuStatus{Physics::GpuState::Unavailable, "no CUDA device"}));

    Rig &rig = Make("NeedsGpu");
    rig.body->backend = SoftBodies::SoftBodyBackend::PhysX;
    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1);
    EXPECT_EQ(warnings.Count("no CUDA device"), 1) << "the reason from the GPU probe is in it";
    EXPECT_EQ(warnings.Count("NeedsGpu"), 1);

    // It still simulates, without another warning
    const std::uint64_t version = rig.body->GetRuntimeMesh()->GetVersion();
    Step(rig.body, 25);
    EXPECT_GT(rig.body->GetRuntimeMesh()->GetVersion(), version);
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1) << "once, not every step";
}

TEST_F(SoftBodyComponentTest, PhysXWithNoPhysicsBackendAtAllAlsoFallsBack)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make();
    rig.body->backend = SoftBodies::SoftBodyBackend::PhysX;
    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1);
}

TEST_F(SoftBodyComponentTest, CpuIsNeverWarnedAbout)
{
    SoftBodyWarningCapture warnings;
    Rig &rig = Make();
    rig.body->backend = SoftBodies::SoftBodyBackend::CPU;
    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_TRUE(warnings.warnings.empty());
}

TEST_F(SoftBodyComponentTest, AutoAndPhysXUseTheGpuBackendTheSceneOffersAndCpuStillWins)
{
    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(false, Physics::GpuStatus{Physics::GpuState::Ready, "fake"});
    fake->withGpuSoftBodies = true;
    TestSupport::FakePhysicsBackend *physics = fake.get();
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    SoftBodyWarningCapture warnings;

    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &autoRig = Make("Auto");
    autoRig.body->backend = SoftBodies::SoftBodyBackend::Auto;
    autoRig.body->OnAttach();
    Rig &physxRig = Make("PhysX");
    physxRig.body->backend = SoftBodies::SoftBodyBackend::PhysX;
    physxRig.body->OnAttach();
    Rig &cpuRig = Make("Cpu");
    cpuRig.body->backend = SoftBodies::SoftBodyBackend::CPU;
    cpuRig.body->OnAttach();

    EXPECT_EQ(autoRig.body->GetActiveBackendName(), "PhysX");
    EXPECT_EQ(physxRig.body->GetActiveBackendName(), "PhysX");
    EXPECT_EQ(cpuRig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(physics->softGpu.created, 2);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore + 2);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    EXPECT_TRUE(warnings.warnings.empty());

    // The simulation really goes through the seam: the component stepped the GPU backend, and the body fell
    const std::uint64_t version = autoRig.body->GetRuntimeMesh()->GetVersion();
    Step(autoRig.body, 10);
    EXPECT_EQ(physics->softGpu.steps, 10);
    EXPECT_GT(autoRig.body->GetRuntimeMesh()->GetVersion(), version);
    EXPECT_LT(MeanPosition(*autoRig.body->GetRuntimeMesh()).y, -0.05f);

    autoRig.body->AddVelocity(Math::Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(physics->softGpu.velocityAdded, 1);

    autoRig.body->OnDestroy();
    physxRig.body->OnDestroy();
    EXPECT_EQ(physics->softGpu.destroyed, 2);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore);
}

TEST_F(SoftBodyComponentTest, ABodyOutlivingItsPhysicsBackendIsInertNotACrash)
{
    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(false, Physics::GpuStatus{Physics::GpuState::Ready, "fake"});
    fake->withGpuSoftBodies = true;
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    Rig &rig = Make();
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "PhysX");
    Step(rig.body, 5);
    const std::shared_ptr<Mesh> runtime = rig.body->GetRuntimeMesh();
    const std::uint64_t version = runtime->GetVersion();

    SoftBodyWarningCapture warnings;
    Application::GetInstance().Set3DPhysicsBackend(nullptr); // the GPU backend is gone
    EXPECT_NO_THROW(Step(rig.body, 5));
    EXPECT_EQ(runtime->GetVersion(), version) << "inert: the picture stays";
    EXPECT_TRUE(warnings.warnings.empty()) << "removed entirely is inert, not a restart";
    EXPECT_NO_THROW(rig.body->AddVelocity(Math::Vector3(1.0f, 0.0f, 0.0f)));
    EXPECT_NO_THROW(rig.body->OnDestroy());
}

TEST_F(SoftBodyComponentTest, AReplacedPhysicsBackendMakesAPhysXBodyRestartOnTheCpuWithOneWarning)
{
    auto first = std::make_unique<TestSupport::FakePhysicsBackend>(false, Physics::GpuStatus{Physics::GpuState::Ready, "fake"});
    first->withGpuSoftBodies = true;
    Application::GetInstance().Set3DPhysicsBackend(std::move(first));
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);

    Rig &rig = Make();
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "PhysX");
    Step(rig.body, 5);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore + 1);

    // The replacement has a soft-body backend of its own; the old handle must not reach it
    auto second = std::make_unique<TestSupport::FakePhysicsBackend>(false, Physics::GpuStatus{Physics::GpuState::Ready, "fake"});
    second->withGpuSoftBodies = true;
    TestSupport::FakePhysicsBackend *secondPtr = second.get();
    Application::GetInstance().Set3DPhysicsBackend(std::move(second));

    SoftBodyWarningCapture warnings;
    Step(rig.body, 5);
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(secondPtr->softGpu.created, 0);
    EXPECT_EQ(secondPtr->softGpu.destroyed, 0);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    EXPECT_EQ(warnings.Count("physics backend changed"), 1);
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), rig.body->GetRuntimeMesh()) << "still drawn";

    // It restarted at rest on the CPU and falls from there
    const float y = MeanPosition(*rig.body->GetRuntimeMesh()).y;
    Step(rig.body, 10);
    EXPECT_LT(MeanPosition(*rig.body->GetRuntimeMesh()).y, y);

    rig.body->OnDestroy();
    EXPECT_EQ(secondPtr->softGpu.destroyed, 0);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);
}

TEST_F(SoftBodyComponentTest, ARecreatedGpuBackendAtTheSameAddressStillInvalidatesTheOldHandle)
{
    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(false, Physics::GpuStatus{Physics::GpuState::Ready, "fake"});
    fake->withGpuSoftBodies = true;
    TestSupport::FakePhysicsBackend *physics = fake.get();
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);

    Rig &rig = Make("Recreated");
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "PhysX");
    Step(rig.body, 5);

    // The very same address now holds a different backend: the old address-only guard would have taken it for the
    // backend that issued the handle, and the handle (index 1 in both) would have named the new backend's first body
    const Physics::ISoftBodyBackend *before = physics->GetSoftBodyBackend();
    const std::uint32_t generationBefore = before->GetGeneration();
    physics->RecreateSoftGpu();
    ASSERT_EQ(physics->GetSoftBodyBackend(), before) << "the test needs the same address";
    EXPECT_NE(physics->GetSoftBodyBackend()->GetGeneration(), generationBefore);

    // A second body on the new backend takes the new backend's first handle index
    Rig &other = Make("Other");
    other.body->OnAttach();
    ASSERT_EQ(other.body->GetActiveBackendName(), "PhysX");
    Step(other.body, 10);
    const float otherY = MeanPosition(*other.body->GetRuntimeMesh()).y;

    SoftBodyWarningCapture warnings;
    Step(rig.body, 5);
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU") << "restarted on the CPU, not attached to the new backend";
    EXPECT_EQ(warnings.Count("physics backend changed"), 1);
    EXPECT_EQ(physics->softGpu.destroyed, 0) << "the old handle never reached the new backend";
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore + 1);
    Step(other.body, 5);
    EXPECT_LT(MeanPosition(*other.body->GetRuntimeMesh()).y, otherY) << "the other body is untouched and falls on";

    rig.body->OnDestroy();
    EXPECT_EQ(physics->softGpu.destroyed, 0);
    other.body->OnDestroy();
    EXPECT_EQ(physics->softGpu.destroyed, 1);
}

TEST_F(SoftBodyComponentTest, AGpuBackendThatRefusesTheBodyFallsBackToTheCpuWithOneWarning)
{
    auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(false, Physics::GpuStatus{Physics::GpuState::Ready, "fake"});
    fake->withGpuSoftBodies = true;
    fake->softGpu.refuseCreate = true;
    TestSupport::FakePhysicsBackend *physics = fake.get();
    Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);

    SoftBodyWarningCapture warnings;
    Rig &rig = Make("Refused");
    rig.body->backend = SoftBodies::SoftBodyBackend::PhysX;
    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("couldn't create the body"), 1);
    EXPECT_EQ(physics->softGpu.created, 0);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);

    const float y = MeanPosition(*rig.body->GetRuntimeMesh()).y;
    Step(rig.body, 10);
    EXPECT_LT(MeanPosition(*rig.body->GetRuntimeMesh()).y, y) << "it simulates on the CPU";
    rig.body->OnDestroy();
}

TEST_F(SoftBodyComponentTest, DestroyingTheObjectWithoutOnDestroyStillGivesTheBackendsCountBack)
{
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    {
        Rig rig;
        rig.object = GameObject::Create("Temp");
        rig.mesh = TestSupport::MakeBoxMesh();
        rig.renderer = rig.object->AddComponent<MeshRenderer>();
        rig.renderer->SetMesh(rig.mesh);
        rig.body = rig.object->AddComponent<SoftBody>();
        rig.body->resolution = 3;
        rig.body->OnAttach();
        EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
        // the object (and its components) go out of scope without OnDestroy, as an object that never joined a scene
    }
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);
}
