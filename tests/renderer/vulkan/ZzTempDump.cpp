#include <gtest/gtest.h>
#include <iostream>
#include "CrossBackendDiff.hpp"
// TEMPORARY: prints the recorded cross-diff lines at the end of the run and fails, so CI shows them. Reverted right after.
namespace {
struct DumpEnv : ::testing::Environment {
  void TearDown() override {
    for (const auto& l : VulkanTesting::Diff::RecordedLines()) std::cout << "XDIFF " << l << std::endl;
    ADD_FAILURE() << "temporary dump";
  }
};
const auto* const registered = ::testing::AddGlobalTestEnvironment(new DumpEnv);
}
