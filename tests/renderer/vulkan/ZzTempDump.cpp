// TEMP: dump the measured cross-diff numbers (reverted right after the CI run)
#include <gtest/gtest.h>

#include <iostream>

#include "CrossBackendDiff.hpp"

namespace
{
    class DumpEnvironment : public ::testing::Environment
    {
    public:
        void TearDown() override
        {
            std::string all;
            for (const auto& line : VulkanTesting::Diff::RecordedLines())
            {
                all += line + "\n";
            }
            ADD_FAILURE() << "TEMPDUMP\n" << all << "ENDDUMP";
        }
    };

    [[maybe_unused]] const auto* const kEnv = ::testing::AddGlobalTestEnvironment(new DumpEnvironment);
}
