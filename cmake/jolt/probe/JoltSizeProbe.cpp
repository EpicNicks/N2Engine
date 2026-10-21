// The web size probe (cmake/jolt/CMakeLists.txt): runs the smoke test's determinism scenario, prints its state hash and
// returns 0 only when it is the golden one. Same program as the baseline plus the part of Jolt that scenario uses.
#include <cstdio>

#include "JoltScenario.hpp"

int main()
{
    const n2jolt::DeterminismResult result = n2jolt::RunDeterminismScenario();
    std::printf("[JOLT-SIZE] probe hash=0x%016llx golden=0x%016llx\n", static_cast<unsigned long long>(result.hash),
                static_cast<unsigned long long>(n2jolt::kGoldenStateHash));
    return (result.stepsOk && result.hash == n2jolt::kGoldenStateHash) ? 0 : 1;
}
