// The physics determinism scene corpus (stage J7 of the Jolt plan, docs/physics.html "Determinism proof in CI").
//
// Fourteen fixed scenes run on the Jolt backend, 600 ticks of 1/60 s each, and the state is hashed every 60 ticks (events and
// query results too, where a scene has them). The hashes are written to a JSON file when the environment variable
// N2_DETERMINISM_HASHES_OUT names one (CI sets it; N2_DETERMINISM_LEG names the leg), and printed on lines that start with
// "[CORPUS]". The CI job "determinism" compares the files of all legs with each other and with tests/golden/
// physics-determinism.json (scripts/physics-determinism.py). Nothing in this file compares a hash with another
// platform's hash: what the tests ASSERT are the properties of one binary in one process (a snapshot round trip and a
// rollback give the straight run's hashes, the same scene run twice gives the same hash). Where PhysX is built, the first
// three scenes also run on it and its hashes go into the file as the "control" (it is expected to differ between legs).
//
// Every scene is deterministic by construction: bodies are made in a fixed order, no container with an unspecified
// iteration order is used, and a random-looking layout comes from a small generator drawn once per statement (the order in
// which the arguments of one call are evaluated is unspecified, so a draw never sits inside an argument list).

#include "PhysicsTestFixture.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Component.hpp"
#include "engine/Layers.hpp"
#include "engine/physics/JointTypes.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr int kTicks = 600;
        constexpr int kInterval = 60;
        constexpr float kStep = 1.0f / 60.0f;

        struct Record
        {
            std::string scene;
            std::string kind;
            int tick = 0;
            std::uint64_t hash = 0;
        };

        std::vector<Record> &JoltRecords()
        {
            static std::vector<Record> records;
            return records;
        }

        std::vector<Record> &ControlRecords()
        {
            static std::vector<Record> records;
            return records;
        }

        std::string Hex(const std::uint64_t value)
        {
            return std::format("0x{:016x}", value);
        }

        // 64-bit FNV-1a over bytes: no std::hash, no library call whose result may differ between platforms
        class Fnv
        {
        public:
            void Byte(const std::uint8_t value)
            {
                _hash ^= static_cast<std::uint64_t>(value);
                _hash *= 1099511628211ULL;
            }
            void U32(const std::uint32_t value)
            {
                for (int i = 0; i < 4; ++i)
                {
                    Byte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
                }
            }
            void U64(const std::uint64_t value)
            {
                for (int i = 0; i < 8; ++i)
                {
                    Byte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
                }
            }
            void Float(const float value)
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                U32(bits);
            }
            void Text(const std::string &text)
            {
                for (const char c : text)
                {
                    Byte(static_cast<std::uint8_t>(c));
                }
                Byte(0xFFu); // a separator, so two texts never run together
            }
            [[nodiscard]] std::uint64_t Value() const { return _hash; }

        private:
            std::uint64_t _hash = 14695981039346656037ULL;
        };

        // The generator of the PhysX determinism tests (#161): one draw per call, in [0, 1)
        class Lcg
        {
        public:
            float Next()
            {
                _state = _state * 6364136223846793005ULL + 1442695040888963407ULL;
                return static_cast<float>((_state >> 40) & 0xFFFFFFULL) / 16777216.0f;
            }

        private:
            std::uint64_t _state = 12345;
        };

        Math::Quaternion TurnedRotation()
        {
            // A unit rotation about no particular axis
            return Math::Quaternion(0.92887f, 0.099f, 0.198f, 0.297f);
        }

        void Add(std::vector<Record> &out, const char *scene, const char *kind, const int tick, const std::uint64_t hash)
        {
            Record record;
            record.scene = scene;
            record.kind = kind;
            record.tick = tick;
            record.hash = hash;
            out.push_back(record);
        }

        // The states of the bodies in the given order, as the bits of the floats (what the PhysX control hashes)
        std::uint64_t HashBodies(IPhysicsBackend &backend, const std::vector<PhysicsBodyHandle> &bodies)
        {
            Fnv fnv;
            for (const PhysicsBodyHandle handle : bodies)
            {
                BodyState state;
                if (!backend.GetBodyState(handle, state))
                {
                    fnv.U32(0xDEADu); // a disabled body is part of the state too
                    continue;
                }
                fnv.Float(state.position.x);
                fnv.Float(state.position.y);
                fnv.Float(state.position.z);
                fnv.Float(state.rotation.GetX());
                fnv.Float(state.rotation.GetY());
                fnv.Float(state.rotation.GetZ());
                fnv.Float(state.rotation.GetW());
                fnv.Float(state.linearVelocity.x);
                fnv.Float(state.linearVelocity.y);
                fnv.Float(state.linearVelocity.z);
                fnv.Float(state.angularVelocity.x);
                fnv.Float(state.angularVelocity.y);
                fnv.Float(state.angularVelocity.z);
                fnv.U32(state.sleeping ? 1u : 0u);
            }
            return fnv.Value();
        }

        // The backend's own state hash (Jolt: HashState); a backend without one is hashed by the body states
        std::uint64_t StateHash(IPhysicsBackend &backend, const std::vector<PhysicsBodyHandle> &bodies)
        {
            const std::optional<std::uint64_t> hash = backend.HashState();
            if (hash.has_value())
            {
                return *hash;
            }
            return HashBodies(backend, bodies);
        }

        // One tick the way Application::PhysicsUpdate runs it (the inputs of the tick are applied before it)
        void Tick(IPhysicsBackend &backend)
        {
            backend.ApplyPendingChanges();
            backend.Update(kStep);
            backend.SyncTransforms();
            backend.ProcessCollisionCallbacks();
        }

        PhysicsBodyHandle AddFloor(IPhysicsBackend &backend, std::vector<PhysicsBodyHandle> &bodies)
        {
            const PhysicsBodyHandle floor =
                backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), Math::Quaternion::Identity, nullptr);
            backend.AddBoxCollider(floor, nullptr, Vector3(50.0f, 0.5f, 50.0f), Vector3::Zero, PhysicsMaterial::Default());
            bodies.push_back(floor);
            return floor;
        }

        // ===== scenes 1 to 3: on every backend (the PhysX control runs them too) =====

        // 1. Free fall: one sphere with a sideways speed, falling onto the floor
        void SceneFreeFall(IPhysicsBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsBodyHandle ball =
                backend.CreateDynamicBody(Vector3(0.0f, 10.0f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
            backend.AddSphereCollider(ball, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
            backend.SetVelocity(ball, Vector3(1.0f, 0.0f, 0.5f));
            bodies.push_back(ball);
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "free_fall", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // 2. A stack of ten boxes with a small alternating offset
        void SceneBoxStack(IPhysicsBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            for (int i = 0; i < 10; ++i)
            {
                const float fi = static_cast<float>(i);
                const float offset = (i % 2 == 0) ? 0.01f * fi : -0.01f * fi;
                const PhysicsBodyHandle box = backend.CreateDynamicBody(
                    Vector3(offset, 0.5f + 1.02f * fi, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, false);
                backend.AddBoxCollider(box, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, PhysicsMaterial::Default());
                bodies.push_back(box);
            }
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "box_stack", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // 3. A pile of 100 mixed boxes, spheres and capsules, laid out by the generator
        void ScenePile(IPhysicsBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            Lcg lcg;
            for (int i = 0; i < 100; ++i)
            {
                const int column = i % 5;
                const int row = (i / 5) % 5;
                const int layer = i / 25;
                // One draw per statement, in this order (see the header of this file)
                const float jitterX = lcg.Next();
                const float jitterY = lcg.Next();
                const float jitterZ = lcg.Next();
                const float spinX = lcg.Next();
                const float spinY = lcg.Next();
                const float spinZ = lcg.Next();
                const float fallSpeed = lcg.Next();
                const float x = 1.3f * static_cast<float>(column - 2) + 0.4f * (jitterX - 0.5f);
                const float y = 1.0f + 1.6f * static_cast<float>(layer) + 0.2f * jitterY;
                const float z = 1.3f * static_cast<float>(row - 2) + 0.4f * (jitterZ - 0.5f);
                const PhysicsBodyHandle body =
                    backend.CreateDynamicBody(Vector3(x, y, z), Math::Quaternion::Identity, 1.0f, nullptr, false);
                switch (i % 3)
                {
                case 0:
                    backend.AddBoxCollider(body, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                    break;
                case 1:
                    backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, material);
                    break;
                default:
                    backend.AddCapsuleCollider(body, nullptr, 0.3f, 1.2f, Vector3::Zero, material);
                    break;
                }
                backend.SetVelocity(body, Vector3(0.0f, -1.0f * fallSpeed, 0.0f));
                backend.SetAngularVelocity(body, Vector3(spinX - 0.5f, spinY - 0.5f, spinZ - 0.5f));
                bodies.push_back(body);
            }
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "pile", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // ===== scenes 4 and 5: Jolt =====

        // 4. Compound bodies: a box, an offset sphere and an offset capsule on one body, with a mass of its own
        void SceneCompound(JoltBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            for (int i = 0; i < 6; ++i)
            {
                const float fi = static_cast<float>(i);
                const Math::Quaternion rotation = (i % 2 == 1) ? TurnedRotation() : Math::Quaternion::Identity;
                const PhysicsBodyHandle body = backend.CreateDynamicBody(
                    Vector3(-6.0f + 2.5f * fi, 2.0f + 0.5f * static_cast<float>(i % 3), 0.0f), rotation, 2.0f, nullptr, false);
                backend.AddBoxCollider(body, nullptr, Vector3(0.5f, 0.3f, 0.4f), Vector3::Zero, material);
                backend.AddSphereCollider(body, nullptr, 0.4f, Vector3(1.0f, 0.2f, 0.0f), material);
                backend.AddCapsuleCollider(body, nullptr, 0.25f, 0.8f, Vector3(-0.8f, 0.0f, 0.3f), material);
                backend.SetMass(body, 5.0f + fi); // the override of the mass the shapes would give
                backend.SetAngularVelocity(body, Vector3(0.0f, 1.0f, 0.5f));
                bodies.push_back(body);
            }
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "compound", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // 5. A kinematic platform sliding along the floor and pushing boxes and spheres in front of it
        void SceneKinematicPlatform(JoltBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            const PhysicsBodyHandle platform =
                backend.CreateDynamicBody(Vector3(-9.0f, 0.6f, 0.0f), Math::Quaternion::Identity, 1.0f, nullptr, true);
            backend.AddBoxCollider(platform, nullptr, Vector3(0.3f, 0.5f, 4.0f), Vector3::Zero, material);
            bodies.push_back(platform);
            for (int i = 0; i < 8; ++i)
            {
                const float fi = static_cast<float>(i);
                const float z = -2.0f + 0.5f * static_cast<float>(i % 5);
                const PhysicsBodyHandle body =
                    backend.CreateDynamicBody(Vector3(-4.0f + 1.5f * fi, 0.6f, z), Math::Quaternion::Identity, 1.0f, nullptr, false);
                if (i % 2 == 0)
                {
                    backend.AddBoxCollider(body, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                }
                else
                {
                    backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, material);
                }
                bodies.push_back(body);
            }
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                const float t = static_cast<float>(tick);
                backend.SetBodyTransform(platform, Vector3(-9.0f + 0.03f * t, 0.6f, 0.0f), Math::Quaternion::Identity);
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "kinematic_platform", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // ===== scene 7: queries =====

        Math::Vector3 Unit(const float x, const float y, const float z)
        {
            const float length = std::sqrt(x * x + y * y + z * z); // IEEE square root: the same on every platform
            return Vector3(x / length, y / length, z / length);
        }

        void HashHandle(Fnv &fnv, const PhysicsBodyHandle handle)
        {
            fnv.U32(handle.index);
            fnv.U32(handle.generation);
        }

        void HashVector(Fnv &fnv, const Vector3 &v)
        {
            fnv.Float(v.x);
            fnv.Float(v.y);
            fnv.Float(v.z);
        }

        void HashHit(Fnv &fnv, const bool found, const RaycastHit &hit)
        {
            fnv.U32(found ? 1u : 0u);
            if (!found)
            {
                return;
            }
            HashVector(fnv, hit.point);
            HashVector(fnv, hit.normal);
            fnv.Float(hit.distance);
            HashHandle(fnv, hit.bodyHandle);
        }

        void HashOverlap(Fnv &fnv, const int count, const std::vector<OverlapHit> &hits)
        {
            fnv.U32(static_cast<std::uint32_t>(count));
            for (const OverlapHit &hit : hits)
            {
                HashHandle(fnv, hit.bodyHandle);
            }
        }

        // The queries of one tick: 64 rays, a raycast-all, a sphere cast, a capsule cast and three overlaps; every result
        // goes into `fnv`
        void RunQueries(JoltBackend &backend, Fnv &fnv)
        {
            const std::uint32_t mask = Layers::DefaultRaycastMask;
            for (int i = 0; i < 64; ++i)
            {
                const Vector3 origin(-9.0f, 0.2f + 0.25f * static_cast<float>(i / 8), -3.5f + 1.0f * static_cast<float>(i % 8));
                const Vector3 direction = (i % 4 == 0) ? Unit(1.0f, -0.1f, 0.05f) : Vector3(1.0f, 0.0f, 0.0f);
                RaycastHit hit;
                const bool found = backend.Raycast(origin, direction, hit, 60.0f, mask, QueryTriggers::Collide);
                HashHit(fnv, found, hit);
            }
            {
                std::vector<RaycastHit> hits;
                const int count = backend.RaycastAll(Vector3(-9.0f, 0.6f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), hits, 60.0f, mask,
                                                     QueryTriggers::Collide);
                fnv.U32(static_cast<std::uint32_t>(count));
                for (const RaycastHit &hit : hits)
                {
                    HashHit(fnv, true, hit);
                }
            }
            {
                RaycastHit hit;
                const bool found = backend.SphereCast(Vector3(-9.0f, 1.0f, 0.5f), 0.3f, Vector3(1.0f, 0.0f, 0.0f), hit, 60.0f, mask);
                HashHit(fnv, found, hit);
            }
            {
                RaycastHit hit;
                const bool found = backend.CapsuleCast(Vector3(-9.0f, 0.6f, -1.0f), Vector3(-9.0f, 1.4f, -1.0f), 0.25f,
                                                       Vector3(1.0f, 0.0f, 0.0f), hit, 60.0f, mask, QueryTriggers::Collide);
                HashHit(fnv, found, hit);
            }
            {
                std::vector<OverlapHit> hits;
                OverlapShape shape;
                shape.kind = OverlapShape::Kind::Sphere;
                shape.center = Vector3(0.0f, 1.0f, 0.0f);
                shape.radius = 2.0f;
                const int count = backend.Overlap(shape, hits, mask, QueryTriggers::Collide);
                HashOverlap(fnv, count, hits);
            }
            {
                std::vector<OverlapHit> hits;
                OverlapShape shape;
                shape.kind = OverlapShape::Kind::Box;
                shape.center = Vector3(2.0f, 1.0f, 1.0f);
                shape.halfExtents = Vector3(1.0f, 1.0f, 1.0f);
                shape.rotation = TurnedRotation();
                const int count = backend.Overlap(shape, hits, mask, QueryTriggers::Collide);
                HashOverlap(fnv, count, hits);
            }
            {
                std::vector<OverlapHit> hits;
                OverlapShape shape;
                shape.kind = OverlapShape::Kind::Capsule;
                shape.point0 = Vector3(-2.0f, 0.5f, 0.0f);
                shape.point1 = Vector3(-2.0f, 2.0f, 0.0f);
                shape.radius = 0.5f;
                const int count = backend.Overlap(shape, hits, mask, QueryTriggers::Collide);
                HashOverlap(fnv, count, hits);
            }
        }

        void SceneQueries(JoltBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            // 4 x 6 bodies on a grid, falling; the rays cross them all the time
            for (int i = 0; i < 24; ++i)
            {
                const float x = -4.0f + 1.6f * static_cast<float>(i % 6);
                const float y = 1.0f + 1.3f * static_cast<float>(i / 12) + 0.15f * static_cast<float>(i % 4);
                const float z = -2.5f + 1.2f * static_cast<float>((i / 6) % 4);
                const PhysicsBodyHandle body =
                    backend.CreateDynamicBody(Vector3(x, y, z), Math::Quaternion::Identity, 1.0f, nullptr, false);
                if (i % 2 == 0)
                {
                    backend.AddSphereCollider(body, nullptr, 0.45f, Vector3::Zero, material);
                }
                else
                {
                    backend.AddBoxCollider(body, nullptr, Vector3(0.4f, 0.4f, 0.4f), Vector3::Zero, material);
                }
                bodies.push_back(body);
            }
            Fnv queries; // every result of every tick so far
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(backend);
                RunQueries(backend, queries);
                if (tick % kInterval == 0)
                {
                    Add(out, "queries", "state", tick, StateHash(backend, bodies));
                    Add(out, "queries", "queries", tick, queries.Value());
                }
            }
        }

        // ===== scene 8: spawn, despawn, enable, disable =====

        void SceneSpawnDespawn(JoltBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies; // the live ones, in creation order
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            int spawned = 0;
            const auto spawn = [&]()
            {
                const float x = -5.0f + 0.9f * static_cast<float>(spawned % 12);
                const float z = -2.0f + 0.8f * static_cast<float>(spawned % 5);
                const float y = 6.0f + 0.3f * static_cast<float>(spawned % 4);
                const PhysicsBodyHandle body =
                    backend.CreateDynamicBody(Vector3(x, y, z), Math::Quaternion::Identity, 1.0f, nullptr, false);
                if (spawned % 2 == 0)
                {
                    backend.AddSphereCollider(body, nullptr, 0.4f, Vector3::Zero, material);
                }
                else
                {
                    backend.AddBoxCollider(body, nullptr, Vector3(0.4f, 0.4f, 0.4f), Vector3::Zero, material);
                }
                bodies.push_back(body);
                ++spawned;
            };
            for (int i = 0; i < 10; ++i)
            {
                spawn();
            }
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                if (tick % 20 == 0)
                {
                    spawn();
                }
                if (tick % 50 == 0 && bodies.size() > 4)
                {
                    backend.DestroyBody(bodies[2]);
                    bodies.erase(bodies.begin() + 2);
                }
                if (tick % 70 == 0)
                {
                    backend.SetBodyEnabled(bodies[1], false);
                }
                if (tick % 70 == 35)
                {
                    backend.SetBodyEnabled(bodies[1], true);
                }
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "spawn_despawn", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // ===== scenes 11 to 14 (#381 item 7): joints, a large pile, fast projectiles, a rollback with joints =====

        // The joint rig of scenes 11 and 14: a chain of six boxes hanging from a world point (a Ball to the world, then
        // limited and free hinges), a slider piston with limits, a welded (Fixed) pair, and a rope (a Distance joint with
        // limits) from a world point to a sphere. Its inputs push the chain's end and the rope's sphere.
        struct JointRig
        {
            std::vector<PhysicsBodyHandle> bodies;
            PhysicsBodyHandle chainEnd;
            PhysicsBodyHandle bob;
            PhysicsBodyHandle piston;

            void Build(JoltBackend &backend)
            {
                const PhysicsMaterial material = PhysicsMaterial::Default();
                AddFloor(backend, bodies);
                const auto box = [&](const Vector3 &at, const Vector3 &half, const float mass)
                {
                    const PhysicsBodyHandle body =
                        backend.CreateDynamicBody(at, Math::Quaternion::Identity, mass, nullptr, false);
                    backend.AddBoxCollider(body, nullptr, half, Vector3::Zero, material);
                    bodies.push_back(body);
                    return body;
                };
                const auto join = [&](const JointType type, const PhysicsBodyHandle a, const Vector3 &posA,
                                      const PhysicsBodyHandle b, const Vector3 &posB, const Vector3 &at, const Vector3 &axis,
                                      const bool limits, const float low, const float high)
                {
                    JointDesc desc;
                    desc.type = type;
                    desc.bodyA = a;
                    desc.bodyB = b;
                    desc.anchorA = at - posA;
                    desc.anchorB = b.IsValid() ? at - posB : at;
                    desc.axis = axis;
                    desc.useLimits = limits;
                    desc.limitMin = low;
                    desc.limitMax = high;
                    const PhysicsJointHandle joint = backend.CreateJoint(desc);
                    EXPECT_TRUE(joint.IsValid());
                };

                // The chain: link 0 hangs from the world at (0, 9, 0)
                Vector3 previousPos(0.0f, 8.6f, 0.0f);
                PhysicsBodyHandle previous = box(previousPos, Vector3(0.15f, 0.35f, 0.15f), 1.0f);
                join(JointType::Ball, previous, previousPos, PhysicsBodyHandle{}, Vector3::Zero, Vector3(0.0f, 9.0f, 0.0f),
                     Vector3(0.0f, 0.0f, 1.0f), false, 0.0f, 0.0f);
                for (int link = 1; link < 6; ++link)
                {
                    const Vector3 position(0.0f, 8.6f - 0.8f * static_cast<float>(link), 0.0f);
                    const PhysicsBodyHandle body = box(position, Vector3(0.15f, 0.35f, 0.15f), 1.0f);
                    const Vector3 at(0.0f, position.y + 0.4f, 0.0f);
                    const bool limited = link % 2 == 1;
                    join(JointType::Hinge, previous, previousPos, body, position, at,
                         (link % 3 == 0) ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f), limited, -0.6f, 0.9f);
                    previous = body;
                    previousPos = position;
                }
                chainEnd = previous;

                // The piston: a base box and a rod that slides along y between -0.2 and 0.4
                const Vector3 basePos(4.0f, 0.5f, 0.0f);
                const PhysicsBodyHandle base = box(basePos, Vector3(0.5f, 0.5f, 0.5f), 4.0f);
                const Vector3 rodPos(4.0f, 1.6f, 0.0f);
                piston = box(rodPos, Vector3(0.15f, 0.6f, 0.15f), 0.5f);
                join(JointType::Slider, base, basePos, piston, rodPos, Vector3(4.0f, 1.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), true,
                     -0.2f, 0.4f);

                // The welded pair, dropped turned onto the floor
                const Vector3 leftPos(-4.0f, 3.0f, 1.0f);
                const Vector3 rightPos(-3.2f, 3.0f, 1.0f);
                const PhysicsBodyHandle left = box(leftPos, Vector3(0.4f, 0.2f, 0.3f), 1.5f);
                const PhysicsBodyHandle right = box(rightPos, Vector3(0.4f, 0.2f, 0.3f), 1.0f);
                join(JointType::Fixed, left, leftPos, right, rightPos, Vector3(-3.6f, 3.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f),
                     false, 0.0f, 0.0f);
                backend.SetAngularVelocity(left, Vector3(0.3f, 0.0f, 0.8f));

                // The rope: a sphere on a Distance joint (0.5 to 2.0) from the world point (2, 6, -2)
                const Vector3 bobPos(3.0f, 5.0f, -2.0f);
                bob = backend.CreateDynamicBody(bobPos, Math::Quaternion::Identity, 0.8f, nullptr, false);
                backend.AddSphereCollider(bob, nullptr, 0.3f, Vector3::Zero, material);
                bodies.push_back(bob);
                join(JointType::Distance, bob, bobPos, PhysicsBodyHandle{}, Vector3::Zero, Vector3(2.0f, 6.0f, -2.0f),
                     Vector3(0.0f, 0.0f, 1.0f), true, 0.5f, 2.0f);
            }

            // The inputs of tick `tick` (1-based); a different variant is a different (wrong) guess of them
            void Input(JoltBackend &backend, const int tick, const int variant) const
            {
                if (tick % 30 == 5)
                {
                    const float sign = ((tick / 30) % 2 == 0) ? 1.0f : -1.0f;
                    backend.AddImpulse(chainEnd, Vector3(sign * (variant == 0 ? 2.0f : 0.5f), 0.0f, 0.8f));
                }
                if (tick % 45 == 20)
                {
                    backend.AddImpulse(bob, variant == 0 ? Vector3(1.5f, 0.5f, 1.0f) : Vector3(-1.0f, 1.0f, 0.0f));
                }
                if (tick % 60 == 30)
                {
                    backend.AddImpulse(piston, Vector3(0.0f, variant == 0 ? 3.0f : 1.0f, 0.0f));
                }
            }
        };

        // 11. Joints: the joint rig with its inputs
        void SceneJoints(JoltBackend &backend, std::vector<Record> &out)
        {
            JointRig rig;
            rig.Build(backend);
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                rig.Input(backend, tick, 0);
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "joints", "state", tick, StateHash(backend, rig.bodies));
                }
            }
        }

        // 12. A large pile: 400 mixed boxes, spheres and capsules in layers of 25 (the size of the bench's largest scene)
        void SceneLargePile(IPhysicsBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            Lcg lcg;
            for (int i = 0; i < 400; ++i)
            {
                const int column = i % 5;
                const int row = (i / 5) % 5;
                const int layer = i / 25;
                const float jitterX = lcg.Next();
                const float jitterZ = lcg.Next();
                const float spin = lcg.Next();
                const float x = 1.3f * static_cast<float>(column - 2) + 0.3f * (jitterX - 0.5f);
                const float y = 1.0f + 1.4f * static_cast<float>(layer);
                const float z = 1.3f * static_cast<float>(row - 2) + 0.3f * (jitterZ - 0.5f);
                const PhysicsBodyHandle body =
                    backend.CreateDynamicBody(Vector3(x, y, z), Math::Quaternion::Identity, 1.0f, nullptr, false);
                switch (i % 3)
                {
                case 0:
                    backend.AddBoxCollider(body, nullptr, Vector3(0.45f, 0.45f, 0.45f), Vector3::Zero, material);
                    break;
                case 1:
                    backend.AddSphereCollider(body, nullptr, 0.45f, Vector3::Zero, material);
                    break;
                default:
                    backend.AddCapsuleCollider(body, nullptr, 0.25f, 1.0f, Vector3::Zero, material);
                    break;
                }
                backend.SetAngularVelocity(body, Vector3(0.0f, spin - 0.5f, 0.0f));
                bodies.push_back(body);
            }
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "large_pile", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // 13. Fast projectiles: every 12 ticks a small sphere is fired at 40 to 70 units per second into a stack of boxes
        // behind a thin static wall with a gap; each lives 150 ticks and is destroyed
        void SceneProjectiles(IPhysicsBackend &backend, std::vector<Record> &out)
        {
            std::vector<PhysicsBodyHandle> bodies;
            AddFloor(backend, bodies);
            const PhysicsMaterial material = PhysicsMaterial::Default();
            for (const float z : {-2.0f, 2.0f})
            {
                const PhysicsBodyHandle wall =
                    backend.CreateStaticBody(Vector3(4.0f, 1.5f, z), Math::Quaternion::Identity, nullptr);
                backend.AddBoxCollider(wall, nullptr, Vector3(0.05f, 1.5f, 1.4f), Vector3::Zero, material);
                bodies.push_back(wall);
            }
            for (int level = 0; level < 5; ++level)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const PhysicsBodyHandle box = backend.CreateDynamicBody(
                        Vector3(7.0f, 0.4f + 0.81f * static_cast<float>(level), -0.8f + 0.8f * static_cast<float>(column)),
                        Math::Quaternion::Identity, 1.0f, nullptr, false);
                    backend.AddBoxCollider(box, nullptr, Vector3(0.4f, 0.4f, 0.4f), Vector3::Zero, material);
                    bodies.push_back(box);
                }
            }
            struct Shot
            {
                PhysicsBodyHandle body;
                int bornAt = 0;
            };
            std::vector<Shot> shots;
            Lcg lcg;
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                if (tick % 12 == 1)
                {
                    const float speedDraw = lcg.Next();
                    const float aimDraw = lcg.Next();
                    const float heightDraw = lcg.Next();
                    const PhysicsBodyHandle shot = backend.CreateDynamicBody(
                        Vector3(-6.0f, 0.5f + 2.5f * heightDraw, 0.0f), Math::Quaternion::Identity, 0.2f, nullptr, false);
                    backend.AddSphereCollider(shot, nullptr, 0.1f, Vector3::Zero, material);
                    backend.SetVelocity(shot, Vector3(40.0f + 30.0f * speedDraw, 1.0f, 3.0f * (aimDraw - 0.5f)));
                    shots.push_back(Shot{shot, tick});
                    bodies.push_back(shot);
                }
                while (!shots.empty() && tick - shots.front().bornAt >= 150)
                {
                    const PhysicsBodyHandle gone = shots.front().body;
                    backend.DestroyBody(gone);
                    std::erase(bodies, gone);
                    shots.erase(shots.begin());
                }
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    Add(out, "projectiles", "state", tick, StateHash(backend, bodies));
                }
            }
        }

        // A deterministic single-threaded Jolt backend, the one every corpus scene runs on
        void InitSingle(JoltBackend &backend)
        {
            backend.SetDeterministic(true);
            EXPECT_TRUE(backend.Initialize());
        }

        // ===== scenes 9 and 10: a world with inputs, for the snapshot and the rollback =====

        struct Mixed
        {
            JoltBackend backend;
            std::vector<PhysicsBodyHandle> bodies;
            PhysicsBodyHandle platform;
            PhysicsBodyHandle ball;

            Mixed()
            {
                backend.SetDeterministic(true);
                EXPECT_TRUE(backend.Initialize());
                Build();
            }

            PhysicsBodyHandle Keep(const PhysicsBodyHandle handle)
            {
                bodies.push_back(handle);
                return handle;
            }

            void Build()
            {
                const PhysicsMaterial material = PhysicsMaterial::Default();
                const Math::Quaternion identity = Math::Quaternion::Identity;
                const PhysicsBodyHandle floor = Keep(backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), identity, nullptr));
                backend.AddBoxCollider(floor, nullptr, Vector3(30.0f, 0.5f, 30.0f), Vector3::Zero, material);
                for (int column = 0; column < 3; ++column)
                {
                    for (int level = 0; level < 4; ++level)
                    {
                        const float x = -4.0f + 2.5f * static_cast<float>(column) + 0.03f * static_cast<float>(level);
                        const float y = 0.5f + 1.02f * static_cast<float>(level);
                        const PhysicsBodyHandle box = Keep(backend.CreateDynamicBody(Vector3(x, y, 0.0f), identity, 1.0f, nullptr, false));
                        backend.AddBoxCollider(box, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                    }
                }
                const PhysicsBodyHandle tilted = Keep(backend.CreateDynamicBody(Vector3(3.0f, 2.0f, 0.0f), TurnedRotation(), 2.0f, nullptr, false));
                backend.AddBoxCollider(tilted, nullptr, Vector3(0.6f, 0.4f, 0.5f), Vector3::Zero, material);
                const PhysicsBodyHandle capsule = Keep(backend.CreateDynamicBody(Vector3(6.0f, 3.0f, 0.0f), TurnedRotation(), 1.5f, nullptr, false));
                backend.AddCapsuleCollider(capsule, nullptr, 0.3f, 1.2f, Vector3::Zero, material);
                const PhysicsBodyHandle compound = Keep(backend.CreateDynamicBody(Vector3(9.0f, 3.0f, 0.0f), identity, 3.0f, nullptr, false));
                backend.AddBoxCollider(compound, nullptr, Vector3(0.5f, 0.5f, 0.5f), Vector3::Zero, material);
                backend.AddSphereCollider(compound, nullptr, 0.5f, Vector3(1.5f, 0.0f, 0.0f), material);
                platform = Keep(backend.CreateDynamicBody(Vector3(-8.0f, 1.0f, 4.0f), identity, 1.0f, nullptr, true));
                backend.AddBoxCollider(platform, nullptr, Vector3(2.0f, 0.2f, 2.0f), Vector3::Zero, material);
                const PhysicsBodyHandle passenger = Keep(backend.CreateDynamicBody(Vector3(-8.0f, 2.0f, 4.0f), identity, 1.0f, nullptr, false));
                backend.AddSphereCollider(passenger, nullptr, 0.4f, Vector3::Zero, material);
                ball = Keep(backend.CreateDynamicBody(Vector3(0.0f, 3.0f, 3.0f), identity, 1.0f, nullptr, false));
                backend.AddSphereCollider(ball, nullptr, 0.5f, Vector3::Zero, material);
                backend.SetVelocity(ball, Vector3(2.0f, 0.0f, 1.0f));
                Lcg lcg;
                for (int i = 0; i < 30; ++i)
                {
                    const float jitterX = lcg.Next();
                    const float jitterZ = lcg.Next();
                    const float x = -6.0f + 1.1f * static_cast<float>(i % 12) + 0.2f * jitterX;
                    const float y = 4.0f + 1.3f * static_cast<float>(i / 12) + 0.2f * static_cast<float>(i % 3);
                    const float z = -3.0f + 0.7f * static_cast<float>(i % 5) + 0.2f * jitterZ;
                    const PhysicsBodyHandle sphere = Keep(backend.CreateDynamicBody(Vector3(x, y, z), identity, 0.5f, nullptr, false));
                    backend.AddSphereCollider(sphere, nullptr, 0.4f, Vector3::Zero, material);
                }
            }

            // The inputs of tick `tick` (1-based): the kinematic target every tick, an impulse on the ball every 40th tick
            // (its direction is the variant) and a gravity change at a tick that is the variant's
            void Input(const int tick, const int variant)
            {
                const float t = static_cast<float>(tick);
                backend.SetBodyTransform(platform, Vector3(-8.0f + 0.02f * t, 1.0f, 4.0f), Math::Quaternion::Identity);
                if (tick % 40 == 10)
                {
                    backend.AddImpulse(ball, variant == 0 ? Vector3(1.5f, 0.0f, 0.0f) : Vector3(-1.0f, 0.5f, 0.7f));
                }
                if (tick == 300 && variant == 0)
                {
                    backend.SetGravity(Vector3(0.5f, -9.81f, 0.0f));
                }
                if (tick == 250 && variant == 1)
                {
                    backend.SetGravity(Vector3(-0.5f, -9.81f, 0.3f));
                }
            }

            void Run(const int tick, const int variant)
            {
                Input(tick, variant);
                Tick(backend);
            }

            [[nodiscard]] std::uint64_t Hash() const
            {
                const std::optional<std::uint64_t> hash = backend.HashState();
                EXPECT_TRUE(hash.has_value());
                return hash.value_or(0);
            }
        };

        // ===== scene 6: events, triggers and layers, in a scene of components =====

        struct EventLog
        {
            int tick = 0;
            std::uint64_t count = 0;
            Fnv hash;
        };
        EventLog *g_events = nullptr;

        // Adds every event its object gets, with the tick it came in, to the log in progress
        class CorpusRecorder final : public Component
        {
        public:
            explicit CorpusRecorder(GameObject &gameObject) : Component(gameObject) {}
            [[nodiscard]] std::string GetTypeName() const override { return "CorpusRecorder"; }

            void OnCollisionEnter(const Collision &c) override { Log("CollisionEnter", c.otherGameObject); }
            void OnCollisionStay(const Collision &c) override { Log("CollisionStay", c.otherGameObject); }
            void OnCollisionExit(const Collision &c) override { Log("CollisionExit", c.otherGameObject); }
            void OnTriggerEnter(const Trigger t) override { Log("TriggerEnter", t.otherGameObject); }
            void OnTriggerStay(const Trigger t) override { Log("TriggerStay", t.otherGameObject); }
            void OnTriggerExit(const Trigger t) override { Log("TriggerExit", t.otherGameObject); }

        private:
            void Log(const char *kind, const GameObject *other) const
            {
                if (g_events)
                {
                    g_events->hash.Text(std::format("{} {} {} {}", g_events->tick, GetGameObject().GetName(), kind,
                                                    other ? other->GetName() : std::string("-")));
                    ++g_events->count;
                }
            }
        };

        constexpr int kOtherLayer = 8;

        void WriteFile()
        {
            const char *path = std::getenv("N2_DETERMINISM_HASHES_OUT");
            if (path == nullptr || *path == '\0')
            {
                return;
            }
            const char *leg = std::getenv("N2_DETERMINISM_LEG");

            nlohmann::json fingerprint;
#if defined(__EMSCRIPTEN__)
            fingerprint["compiler"] = std::string("emscripten clang ") + __clang_version__;
            fingerprint["arch"] = "wasm32";
#elif defined(_MSC_VER)
            fingerprint["compiler"] = "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
            fingerprint["compiler"] = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
            fingerprint["compiler"] = std::string("gcc ") + __VERSION__;
#else
            fingerprint["compiler"] = "unknown";
#endif
#if !defined(__EMSCRIPTEN__)
#if defined(_M_X64) || defined(__x86_64__)
            fingerprint["arch"] = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
            fingerprint["arch"] = "arm64";
#else
            fingerprint["arch"] = "unknown";
#endif
#endif
            nlohmann::json flags;
#ifdef N2_STRICT_FP
            flags["strict_fp"] = true;
#else
            flags["strict_fp"] = false;
#endif
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
            flags["jolt_cross_platform_deterministic"] = true;
#else
            flags["jolt_cross_platform_deterministic"] = false;
#endif
#ifdef N2ENGINE_SINGLE_THREADED
            flags["single_threaded_build"] = true;
#else
            flags["single_threaded_build"] = false;
#endif
#ifdef NDEBUG
            flags["ndebug"] = true;
#else
            flags["ndebug"] = false;
#endif
            flags["declared_determinism"] = std::string(ToString(JoltBackend::DeclaredDeterminism(true)));
            fingerprint["flags"] = flags;

            const auto toJson = [](const std::vector<Record> &records)
            {
                nlohmann::json array = nlohmann::json::array();
                for (const Record &record : records)
                {
                    array.push_back(nlohmann::json{
                        {"scene", record.scene}, {"kind", record.kind}, {"tick", record.tick}, {"hash", Hex(record.hash)}});
                }
                return array;
            };

            nlohmann::json root;
            root["format"] = 1;
            root["leg"] = (leg != nullptr && *leg != '\0') ? std::string(leg) : std::string("local");
            root["ticks"] = kTicks;
            root["interval"] = kInterval;
            root["fingerprint"] = fingerprint;
            root["backend"] = "jolt";
            root["records"] = toJson(JoltRecords());
            if (!ControlRecords().empty())
            {
                root["control"] = nlohmann::json{{"backend", "physx"}, {"records", toJson(ControlRecords())}};
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << root.dump(1) << "\n";
            out.flush();
            if (!out)
            {
                ADD_FAILURE() << "could not write " << path;
            }
        }

        void Print(const char *backend, const std::vector<Record> &records)
        {
            for (const Record &record : records)
            {
                std::printf("[CORPUS] %s %s %s tick %d %s\n", backend, record.scene.c_str(), record.kind.c_str(), record.tick,
                            Hex(record.hash).c_str());
            }
            std::fflush(stdout);
        }
    }

    class PhysicsDeterminismCorpusTest : public PhysicsCoreFixture<JoltTag>
    {
    public:
        static void SetUpTestSuite()
        {
            // The suite's own backend, single-threaded like every other scene (the base installs a thread-pool one)
            auto backend = std::make_unique<JoltBackend>();
            backend->SetDeterministic(true);
            ASSERT_TRUE(backend->Initialize());
            Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
            JoltRecords().clear();
            ControlRecords().clear();
        }

        // After every test of the suite: print the records and write the file (a failed test leaves its records out,
        // which the comparison script reports as missing)
        static void TearDownTestSuite()
        {
            Print("jolt", JoltRecords());
            Print("physx", ControlRecords());
            WriteFile();
            PhysicsCoreFixture<JoltTag>::TearDownTestSuite();
        }

    protected:
        void SetUp() override
        {
            Layers::ResetToDefaults();
            PhysicsCoreFixture<JoltTag>::SetUp();
        }

        void TearDown() override
        {
            g_events = nullptr;
            PhysicsCoreFixture<JoltTag>::TearDown();
            Layers::ResetToDefaults();
        }
    };

    // ===== the scenes =====

    TEST_F(PhysicsDeterminismCorpusTest, Scene01FreeFall)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        SceneFreeFall(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene02BoxStack)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        SceneBoxStack(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene03Pile)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        ScenePile(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene04Compound)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        SceneCompound(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene05KinematicPlatform)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        SceneKinematicPlatform(backend, JoltRecords());
    }

    // 6. Triggers and layers: balls on two layers fall into a trigger zone and onto a floor; the collision matrix between the
    // layers is switched off for a few ticks twice, and one ball changes layer. The events (tick, object, kind, other
    // object) are hashed in the order they arrive.
    TEST_F(PhysicsDeterminismCorpusTest, Scene06TriggersAndLayers)
    {
        EventLog log;
        g_events = &log;
        const auto floor = SpawnFloor();
        floor->AddComponent<CorpusRecorder>();
        const auto zone = Spawn("Zone", Vector3(0.0f, 2.0f, 0.0f));
        auto *zoneCollider = zone->AddComponent<BoxCollider>();
        zoneCollider->SetSize(Vector3(6.0f, 3.0f, 6.0f));
        zoneCollider->SetIsTrigger(true);
        zone->AddComponent<CorpusRecorder>();
        std::vector<GameObject::Ptr> balls;
        for (int i = 0; i < 8; ++i)
        {
            const float fi = static_cast<float>(i);
            const GameObject::Ptr ball = Spawn(std::format("Ball{}", i),
                                               Vector3(-3.5f + 1.0f * fi, 6.0f + 0.7f * static_cast<float>(i % 4),
                                                       -1.0f + 0.5f * static_cast<float>(i % 3)));
            if (i % 2 == 1)
            {
                ball->SetLayer(kOtherLayer); // before the colliders are added, as LayerPhysicsTests does
            }
            ball->AddComponent<SphereCollider>()->SetRadius(0.5f);
            auto *body = ball->AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(true);
            ball->AddComponent<CorpusRecorder>();
            balls.push_back(ball);
        }
        for (int tick = 1; tick <= kTicks; ++tick)
        {
            log.tick = tick;
            if (tick == 100 || tick == 300)
            {
                Layers::SetCollision(Layers::Default, kOtherLayer, false);
            }
            if (tick == 108 || tick == 306)
            {
                Layers::SetCollision(Layers::Default, kOtherLayer, true);
            }
            if (tick == 200)
            {
                balls[2]->SetLayer(kOtherLayer);
            }
            Step(1);
            if (tick % kInterval == 0)
            {
                Add(JoltRecords(), "triggers_layers", "state", tick, Native().HashState().value_or(0));
                Add(JoltRecords(), "triggers_layers", "events", tick, log.hash.Value());
            }
        }
        EXPECT_GT(log.count, 0u) << "the scene raised no event, so the event hash proves nothing";
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene07Queries)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        SceneQueries(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene08SpawnDespawn)
    {
        JoltBackend backend;
        backend.SetDeterministic(true);
        ASSERT_TRUE(backend.Initialize());
        SceneSpawnDespawn(backend, JoltRecords());
    }

    // 9. Snapshot round trip: save at tick 200, run to 400 (H1), restore, run to 400 again (H2): H1 == H2, and both equal
    // the hash of a world that ran straight through. The recorded "state" hashes are the straight world's.
    TEST_F(PhysicsDeterminismCorpusTest, Scene09SnapshotRoundTrip)
    {
        Mixed straight;
        std::vector<std::uint64_t> straightHashes(kTicks / kInterval + 1, 0);
        // Tick 400 is not on the 60-tick hash grid, so the straight world's hash there is taken on its own
        std::uint64_t straightAt400 = 0;
        for (int tick = 1; tick <= kTicks; ++tick)
        {
            straight.Run(tick, 0);
            if (tick == 400)
            {
                straightAt400 = straight.Hash();
            }
            if (tick % kInterval == 0)
            {
                const std::uint64_t hash = straight.Hash();
                straightHashes[static_cast<std::size_t>(tick / kInterval)] = hash;
                Add(JoltRecords(), "snapshot_roundtrip", "state", tick, hash);
            }
        }

        Mixed world;
        for (int tick = 1; tick <= 200; ++tick)
        {
            world.Run(tick, 0);
        }
        std::vector<std::byte> saved;
        ASSERT_TRUE(world.backend.SaveState(saved));
        EXPECT_FALSE(saved.empty());
        const std::uint64_t hashAtSave = world.Hash();
        for (int tick = 201; tick <= 400; ++tick)
        {
            world.Run(tick, 0);
        }
        const std::uint64_t h1 = world.Hash();
        ASSERT_TRUE(world.backend.RestoreState(saved));
        EXPECT_EQ(world.Hash(), hashAtSave) << "the restore did not bring the saved state back";
        for (int tick = 201; tick <= 400; ++tick)
        {
            world.Run(tick, 0);
        }
        const std::uint64_t h2 = world.Hash();
        Add(JoltRecords(), "snapshot_roundtrip", "snapshot_h1", 400, h1);
        Add(JoltRecords(), "snapshot_roundtrip", "snapshot_h2", 400, h2);
        EXPECT_EQ(h1, h2) << "the second run of ticks 201 to 400 differs from the first";
        EXPECT_EQ(h1, straightAt400) << "the snapshot world differs from the straight run at tick 400";
        for (int tick = 401; tick <= kTicks; ++tick)
        {
            world.Run(tick, 0);
        }
        EXPECT_EQ(world.Hash(), straightHashes[kTicks / kInterval]) << "the restored world differs from the straight run at the end";
    }

    // 10. Rollback simulation: every 10 ticks the world is saved, 8 ticks are run (a prediction), the state is restored and
    // the 10 ticks are run again with the same inputs. Its hashes must equal a straight run's at every recorded tick.
    TEST_F(PhysicsDeterminismCorpusTest, Scene10Rollback)
    {
        Mixed straight;
        std::vector<std::uint64_t> straightHashes(kTicks / kInterval + 1, 0);
        for (int tick = 1; tick <= kTicks; ++tick)
        {
            straight.Run(tick, 1);
            if (tick % kInterval == 0)
            {
                straightHashes[static_cast<std::size_t>(tick / kInterval)] = straight.Hash();
            }
        }

        Mixed world;
        for (int base = 0; base < kTicks; base += 10)
        {
            std::vector<std::byte> saved;
            ASSERT_TRUE(world.backend.SaveState(saved));
            for (int tick = base + 1; tick <= base + 8; ++tick)
            {
                world.Run(tick, 1);
            }
            ASSERT_TRUE(world.backend.RestoreState(saved));
            for (int tick = base + 1; tick <= base + 10; ++tick)
            {
                world.Run(tick, 1);
            }
            const int done = base + 10;
            if (done % kInterval == 0)
            {
                const std::uint64_t hash = world.Hash();
                Add(JoltRecords(), "rollback", "state", done, hash);
                EXPECT_EQ(hash, straightHashes[static_cast<std::size_t>(done / kInterval)])
                    << "the rolled-back world differs from the straight run at tick " << done;
            }
        }
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene11Joints)
    {
        JoltBackend backend;
        InitSingle(backend);
        SceneJoints(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene12LargePile)
    {
        JoltBackend backend;
        InitSingle(backend);
        SceneLargePile(backend, JoltRecords());
    }

    TEST_F(PhysicsDeterminismCorpusTest, Scene13Projectiles)
    {
        JoltBackend backend;
        InitSingle(backend);
        SceneProjectiles(backend, JoltRecords());
    }

    // 14. Rollback with joints: scene 10's save, mispredict for 8 ticks, restore and run 10 ticks again, on the joint rig
    // of scene 11. Its hashes must equal a straight run's at every recorded tick; the recorded ones are the rolled-back
    // world's.
    TEST_F(PhysicsDeterminismCorpusTest, Scene14JointRollback)
    {
        std::vector<std::uint64_t> straightHashes(kTicks / kInterval + 1, 0);
        {
            JoltBackend backend;
            InitSingle(backend);
            JointRig rig;
            rig.Build(backend);
            for (int tick = 1; tick <= kTicks; ++tick)
            {
                rig.Input(backend, tick, 0);
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    straightHashes[static_cast<std::size_t>(tick / kInterval)] = backend.HashState().value_or(0);
                }
            }
        }
        JoltBackend backend;
        InitSingle(backend);
        JointRig rig;
        rig.Build(backend);
        for (int base = 0; base < kTicks; base += 10)
        {
            std::vector<std::byte> saved;
            ASSERT_TRUE(backend.SaveState(saved));
            for (int tick = base + 1; tick <= base + 8; ++tick)
            {
                rig.Input(backend, tick, 1);
                Tick(backend);
            }
            ASSERT_TRUE(backend.RestoreState(saved));
            for (int tick = base + 1; tick <= base + 10; ++tick)
            {
                rig.Input(backend, tick, 0);
                Tick(backend);
            }
            const int done = base + 10;
            if (done % kInterval == 0)
            {
                const std::uint64_t hash = backend.HashState().value_or(0);
                Add(JoltRecords(), "joint_rollback", "state", done, hash);
                EXPECT_EQ(hash, straightHashes[static_cast<std::size_t>(done / kInterval)])
                    << "the rolled-back joint rig differs from the straight run at tick " << done;
            }
        }
    }

    // ===== #381 item 7: the corpus scenes do not depend on Jolt's worker thread count =====

    // Each scene that runs on a backend of its own (all but 6, which runs on the Application's, and 9 and 10, whose world
    // is the rollback test's) is run on the single-threaded job system the corpus uses and on the thread-pool job system
    // with 1 and 3 worker threads, in this process. Every recorded hash must be the same. These hashes are not written to
    // the corpus file and are not compared with other legs: what this proves is that one binary gives the same state with
    // any worker count; the preset still runs single-threaded (docs/physics.html). Skipped on the web (no thread pool).
    TEST(PhysicsCorpusThreadsTest, TheCorpusScenesGiveTheSameHashesWithAnyWorkerThreadCount)
    {
#ifdef N2ENGINE_SINGLE_THREADED
        GTEST_SKIP() << "the web build has no thread pool: the single-threaded job system is the only one";
#else
        using SceneFn = void (*)(JoltBackend &, std::vector<Record> &);
        struct Entry
        {
            const char *name;
            SceneFn run;
        };
        const Entry scenes[] = {
            {"free_fall", [](JoltBackend &b, std::vector<Record> &o) { SceneFreeFall(b, o); }},
            {"box_stack", [](JoltBackend &b, std::vector<Record> &o) { SceneBoxStack(b, o); }},
            {"pile", [](JoltBackend &b, std::vector<Record> &o) { ScenePile(b, o); }},
            {"compound", [](JoltBackend &b, std::vector<Record> &o) { SceneCompound(b, o); }},
            {"kinematic_platform", [](JoltBackend &b, std::vector<Record> &o) { SceneKinematicPlatform(b, o); }},
            {"queries", [](JoltBackend &b, std::vector<Record> &o) { SceneQueries(b, o); }},
            {"spawn_despawn", [](JoltBackend &b, std::vector<Record> &o) { SceneSpawnDespawn(b, o); }},
            {"joints", [](JoltBackend &b, std::vector<Record> &o) { SceneJoints(b, o); }},
            {"large_pile", [](JoltBackend &b, std::vector<Record> &o) { SceneLargePile(b, o); }},
            {"projectiles", [](JoltBackend &b, std::vector<Record> &o) { SceneProjectiles(b, o); }},
        };
        std::size_t compared = 0;
        for (const Entry &scene : scenes)
        {
            std::vector<Record> single;
            {
                JoltBackend backend;
                InitSingle(backend);
                ASSERT_TRUE(backend.UsesSingleThreadedJobSystem());
                scene.run(backend, single);
            }
            ASSERT_FALSE(single.empty()) << scene.name;
            for (const int workers : {1, 3})
            {
                std::vector<Record> pooled;
                JoltBackend backend;
                backend.SetDeterministic(false);
                backend.SetWorkerThreadsForTesting(workers);
                ASSERT_TRUE(backend.Initialize());
                ASSERT_FALSE(backend.UsesSingleThreadedJobSystem());
                scene.run(backend, pooled);
                ASSERT_EQ(pooled.size(), single.size()) << scene.name << ", " << workers << " workers";
                for (std::size_t i = 0; i < single.size(); ++i)
                {
                    EXPECT_EQ(pooled[i].hash, single[i].hash) << scene.name << " " << single[i].kind << " tick " << single[i].tick
                                                              << " with " << workers << " worker threads";
                    ++compared;
                }
                std::printf("[CORPUS-THREADS] %s workers=%d concurrency=%d records=%zu last=%s single=%s\n", scene.name, workers,
                            backend.GetJobSystemConcurrency(), pooled.size(), Hex(pooled.back().hash).c_str(),
                            Hex(single.back().hash).c_str());
                std::fflush(stdout);
            }
        }
        EXPECT_GE(compared, 200u);
#endif
    }

    // The joint rollback of scene 14 under worker threads: saved, mispredicted, restored and run again on a 3-worker pool,
    // it reaches the single-threaded straight run's hashes
    TEST(PhysicsCorpusThreadsTest, AJointRollbackUnderWorkerThreadsReachesTheSingleThreadedStraightRun)
    {
#ifdef N2ENGINE_SINGLE_THREADED
        GTEST_SKIP() << "the web build has no thread pool";
#else
        std::vector<std::uint64_t> straight;
        {
            JoltBackend backend;
            InitSingle(backend);
            JointRig rig;
            rig.Build(backend);
            for (int tick = 1; tick <= 300; ++tick)
            {
                rig.Input(backend, tick, 0);
                Tick(backend);
                if (tick % kInterval == 0)
                {
                    straight.push_back(backend.HashState().value_or(0));
                }
            }
        }
        JoltBackend backend;
        backend.SetDeterministic(false);
        backend.SetWorkerThreadsForTesting(3);
        ASSERT_TRUE(backend.Initialize());
        JointRig rig;
        rig.Build(backend);
        std::vector<std::uint64_t> rolled;
        for (int base = 0; base < 300; base += 10)
        {
            std::vector<std::byte> saved;
            ASSERT_TRUE(backend.SaveState(saved));
            for (int tick = base + 1; tick <= base + 8; ++tick)
            {
                rig.Input(backend, tick, 1);
                Tick(backend);
            }
            ASSERT_TRUE(backend.RestoreState(saved));
            for (int tick = base + 1; tick <= base + 10; ++tick)
            {
                rig.Input(backend, tick, 0);
                Tick(backend);
            }
            if ((base + 10) % kInterval == 0)
            {
                rolled.push_back(backend.HashState().value_or(0));
            }
        }
        EXPECT_EQ(rolled, straight);
#endif
    }

#ifdef N2ENGINE_PHYSX_ENABLED
    // The same-leg negative control: scenes 1 to 3 on PhysX, whose hashes are expected to differ between legs (the
    // comparison script reports whether they do). They are recorded as the "control" and never compared with the goldens.
    TEST_F(PhysicsDeterminismCorpusTest, ControlPhysXScenes)
    {
        {
            PhysXBackend backend;
            backend.SetDeterministic(true);
            ASSERT_TRUE(backend.Initialize());
            SceneFreeFall(backend, ControlRecords());
        }
        {
            PhysXBackend backend;
            backend.SetDeterministic(true);
            ASSERT_TRUE(backend.Initialize());
            SceneBoxStack(backend, ControlRecords());
        }
        {
            PhysXBackend backend;
            backend.SetDeterministic(true);
            ASSERT_TRUE(backend.Initialize());
            ScenePile(backend, ControlRecords());
        }
        EXPECT_EQ(ControlRecords().size(), 30u);
    }
#endif
}

#endif // N2ENGINE_JOLT_ENABLED
