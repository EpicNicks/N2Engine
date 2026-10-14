#include <gtest/gtest.h>

#include <cstdint>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "engine/physics/BackendHandle.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"

// The generation-counted handle helper shared by the particle and soft-body backends (BackendHandle.hpp), and the
// ISoftBodyBackend seam built on it. No physics scene, no GPU: pure logic.

using namespace N2Engine;
using namespace N2Engine::Physics;

namespace
{
    struct TagA;
    struct TagB;

    /// The smallest soft-body backend: one body per CreateBody, positions are the body's index
    class FakeSoftBodyBackend final : public ISoftBodyBackend
    {
    public:
        [[nodiscard]] std::string_view GetName() const override { return "Fake"; }

        [[nodiscard]] SoftBodyHandle CreateBody(const SoftBodyDesc &) override
        {
            alive.push_back(true);
            return MakeSoftBodyHandle(GetGeneration(), static_cast<std::uint32_t>(alive.size() - 1));
        }

        void Destroy(const SoftBodyHandle handle) override
        {
            if (Valid(handle))
            {
                alive[SoftBodyHandleIndex(handle)] = false;
            }
        }

        void SetParams(const SoftBodyHandle handle, const SoftBodyParams &params) override
        {
            if (Valid(handle))
            {
                lastParams = params;
                ++setParamsCalls;
            }
        }

        void Step(const float) override {}

        void CopySurfacePositions(const SoftBodyHandle handle, std::vector<Math::Vector3> &out) const override
        {
            out.clear();
            if (Valid(handle))
            {
                out.emplace_back(static_cast<float>(SoftBodyHandleIndex(handle)), 0.0f, 0.0f);
            }
        }

        [[nodiscard]] bool Valid(const SoftBodyHandle handle) const
        {
            return OwnsHandle(handle) && SoftBodyHandleIndex(handle) < alive.size() &&
                   alive[SoftBodyHandleIndex(handle)];
        }

        std::vector<bool> alive;
        SoftBodyParams lastParams;
        int setParamsCalls = 0;
    };
}

TEST(BackendHandleTest, PacksTheGenerationAndTheIndex)
{
    const BackendHandle handle = MakeBackendHandle(7u, 42u);
    EXPECT_EQ(BackendHandleGeneration(handle), 7u);
    EXPECT_EQ(BackendHandleIndex(handle), 42u);
    EXPECT_EQ(handle, (static_cast<std::uint64_t>(7) << 32) | 42u);

    const BackendHandle extreme = MakeBackendHandle(0xFFFFFFFFu, 0xFFFFFFFFu);
    EXPECT_EQ(BackendHandleGeneration(extreme), 0xFFFFFFFFu);
    EXPECT_EQ(BackendHandleIndex(extreme), 0xFFFFFFFFu);
}

TEST(BackendHandleTest, ZeroIsNeverOwned)
{
    EXPECT_EQ(kInvalidBackendHandle, 0u);
    EXPECT_FALSE(BackendHandleOwnedBy(1u, kInvalidBackendHandle));
    // Generation 0 is never issued, so even a handle with index 0 and generation 0 (the value 0) is no one's
    EXPECT_FALSE(BackendHandleOwnedBy(0u, MakeBackendHandle(0u, 0u)));
    // Index 0 of a real generation is a valid handle
    EXPECT_TRUE(BackendHandleOwnedBy(3u, MakeBackendHandle(3u, 0u)));
    EXPECT_FALSE(BackendHandleOwnedBy(3u, MakeBackendHandle(4u, 0u)));
}

TEST(BackendHandleTest, GenerationsCountUpFromOneAndEachFeatureHasItsOwnCounter)
{
    const std::uint32_t a1 = NextBackendGeneration<TagA>();
    const std::uint32_t b1 = NextBackendGeneration<TagB>();
    const std::uint32_t a2 = NextBackendGeneration<TagA>();
    const std::uint32_t a3 = NextBackendGeneration<TagA>();
    const std::uint32_t b2 = NextBackendGeneration<TagB>();
    EXPECT_GE(a1, 1u) << "0 is never issued";
    EXPECT_GE(b1, 1u);
    EXPECT_EQ(a2, a1 + 1);
    EXPECT_EQ(a3, a2 + 1);
    EXPECT_EQ(b2, b1 + 1) << "another feature's calls do not advance this counter";
}

TEST(BackendHandleTest, ParticleHandlesAreTheSharedHelperWithTheirOwnNames)
{
    static_assert(std::is_same_v<ParticleSystemHandle, BackendHandle>);
    static_assert(std::is_same_v<SoftBodyHandle, BackendHandle>);
    EXPECT_EQ(MakeParticleHandle(5u, 9u), MakeBackendHandle(5u, 9u));
    EXPECT_EQ(MakeSoftBodyHandle(5u, 9u), MakeBackendHandle(5u, 9u));
    EXPECT_EQ(ParticleHandleGeneration(MakeParticleHandle(5u, 9u)), 5u);
    EXPECT_EQ(ParticleHandleIndex(MakeParticleHandle(5u, 9u)), 9u);
    EXPECT_EQ(kInvalidParticleSystem, kInvalidBackendHandle);
    EXPECT_EQ(kInvalidSoftBody, kInvalidBackendHandle);

    // Particle generations keep counting 1, 2, 3 ... on their own counter, whatever the soft-body backends do
    const std::uint32_t first = NextParticleBackendGeneration();
    const FakeSoftBodyBackend soft1;
    const FakeSoftBodyBackend soft2;
    EXPECT_NE(soft1.GetGeneration(), soft2.GetGeneration());
    EXPECT_EQ(NextParticleBackendGeneration(), first + 1);
}

TEST(SoftBodySeamTest, EachBackendObjectHasItsOwnGeneration)
{
    FakeSoftBodyBackend first;
    FakeSoftBodyBackend second;
    EXPECT_NE(first.GetGeneration(), 0u);
    EXPECT_NE(second.GetGeneration(), 0u);
    EXPECT_NE(first.GetGeneration(), second.GetGeneration());

    const SoftBodyHandle handle = first.CreateBody({});
    ASSERT_NE(handle, kInvalidSoftBody);
    EXPECT_EQ(SoftBodyHandleGeneration(handle), first.GetGeneration());
    EXPECT_TRUE(first.OwnsHandle(handle));
    EXPECT_FALSE(second.OwnsHandle(handle)) << "another backend does not know it";
    EXPECT_FALSE(first.OwnsHandle(kInvalidSoftBody));
}

TEST(SoftBodySeamTest, AHandleOfAnotherBackendIsIgnoredEvenAtTheSameAddress)
{
    alignas(FakeSoftBodyBackend) unsigned char storage[sizeof(FakeSoftBodyBackend)];
    auto *backend = new (storage) FakeSoftBodyBackend();
    const SoftBodyHandle old = backend->CreateBody({});
    const std::uint32_t oldGeneration = backend->GetGeneration();
    ASSERT_TRUE(backend->Valid(old));

    // Destroyed and constructed again in the same place: the same address, a new backend, a new generation
    backend->~FakeSoftBodyBackend();
    auto *replacement = new (storage) FakeSoftBodyBackend();
    ASSERT_EQ(replacement, backend) << "the test needs the same address";
    EXPECT_NE(replacement->GetGeneration(), oldGeneration);
    EXPECT_GT(replacement->GetGeneration(), oldGeneration);

    // The replacement has a body at index 0 too, and the old handle must not name it
    const SoftBodyHandle fresh = replacement->CreateBody({});
    ASSERT_EQ(SoftBodyHandleIndex(fresh), SoftBodyHandleIndex(old));
    EXPECT_FALSE(replacement->OwnsHandle(old));
    EXPECT_FALSE(replacement->Valid(old));
    EXPECT_TRUE(replacement->Valid(fresh));

    replacement->Destroy(old);
    EXPECT_TRUE(replacement->Valid(fresh)) << "destroying a stale handle changes nothing";
    replacement->SetParams(old, SoftBodyParams{});
    EXPECT_EQ(replacement->setParamsCalls, 0);
    std::vector<Math::Vector3> out{Math::Vector3(1.0f, 2.0f, 3.0f)};
    replacement->CopySurfacePositions(old, out);
    EXPECT_TRUE(out.empty());
    replacement->CopySurfacePositions(fresh, out);
    EXPECT_EQ(out.size(), 1u);

    replacement->~FakeSoftBodyBackend();
}

TEST(SoftBodySeamTest, TheParticleSeamKeepsItsGenerationSemantics)
{
    // The particle backend interface is built on the same helper and behaves as before: handles of another
    // generation are not owned, 0 is never owned
    struct Probe final : IParticleBackend
    {
        [[nodiscard]] std::string_view GetName() const override { return "Probe"; }
        [[nodiscard]] ParticleSystemHandle CreateSystem(const ParticleSystemDesc &) override
        {
            return MakeParticleHandle(GetGeneration(), 0);
        }
        void Destroy(ParticleSystemHandle) override {}
        void SetParams(ParticleSystemHandle, const ParticleParams &) override {}
        std::size_t Spawn(ParticleSystemHandle, std::span<const Math::Vector3>, std::span<const Math::Vector3>) override
        {
            return 0;
        }
        void Clear(ParticleSystemHandle) override {}
        void Step(float) override {}
        [[nodiscard]] std::size_t GetAliveCount(ParticleSystemHandle) const override { return 0; }
        void CopyPositions(ParticleSystemHandle, std::vector<Math::Vector4> &) const override {}
    };

    Probe first;
    Probe second;
    const ParticleSystemHandle handle = first.CreateSystem({});
    EXPECT_TRUE(first.OwnsHandle(handle));
    EXPECT_FALSE(second.OwnsHandle(handle));
    EXPECT_FALSE(first.OwnsHandle(kInvalidParticleSystem));
    EXPECT_EQ(ParticleHandleGeneration(handle), first.GetGeneration());
    EXPECT_EQ(second.GetGeneration(), first.GetGeneration() + 1);
}
