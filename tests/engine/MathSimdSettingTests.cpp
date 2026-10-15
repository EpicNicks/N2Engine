#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <string>
#include <vector>

#include <math/MathRegistrar.hpp>
#include <nlohmann/json.hpp>

#include "engine/ProjectSettings.hpp"

// The project setting math.simdTier: parsing, defaults, and validation through ApplyProjectSettings. Application::Init
// reads it with ReadMathSimdTier before it chooses the math tier (tests never call Init, so the choice itself is
// covered by SimdTierSettingTest in math_tests).

using namespace N2Engine;
using nlohmann::json;
using Math::SIMDTierRequest;

TEST(MathSimdSettingTest, DefaultsToAuto)
{
    std::vector<std::string> problems;
    EXPECT_EQ(ReadMathSimdTier(json::object(), &problems), SIMDTierRequest::Auto);
    EXPECT_EQ(ReadMathSimdTier(json{{"math", json::object()}}, &problems), SIMDTierRequest::Auto);
    EXPECT_EQ(ReadMathSimdTier(json{{"math", {{"simdTier", nullptr}}}}, &problems), SIMDTierRequest::Auto);
    EXPECT_EQ(ReadMathSimdTier(json::array(), &problems), SIMDTierRequest::Auto);
    EXPECT_EQ(ReadMathSimdTier(json{{"math", 3}}, &problems), SIMDTierRequest::Auto);
    EXPECT_TRUE(problems.empty());
}

TEST(MathSimdSettingTest, ReadsEachTierName)
{
    const std::pair<const char *, SIMDTierRequest> cases[] = {
        {"auto", SIMDTierRequest::Auto},   {"scalar", SIMDTierRequest::Scalar}, {"sse2", SIMDTierRequest::SSE2},
        {"sse4.1", SIMDTierRequest::SSE41}, {"avx", SIMDTierRequest::AVX},       {"neon", SIMDTierRequest::NEON},
        {"Scalar", SIMDTierRequest::Scalar}, {"AVX", SIMDTierRequest::AVX},
    };
    for (const auto &[name, expected] : cases)
    {
        // Saved as the project file's settings, read back
        const json reloaded = json::parse(json{{"math", {{"simdTier", name}}}}.dump());
        std::vector<std::string> problems;
        EXPECT_EQ(ReadMathSimdTier(reloaded, &problems), expected) << name;
        EXPECT_TRUE(problems.empty()) << name;
    }
}

TEST(MathSimdSettingTest, UnknownValueIsAWarningAndMeansAuto)
{
    for (const json &bad : {json("avx512"), json(true), json(1), json::object(), json("")})
    {
        std::vector<std::string> problems;
        EXPECT_EQ(ReadMathSimdTier(json{{"math", {{"simdTier", bad}}}}, &problems), SIMDTierRequest::Auto)
            << bad.dump();
        ASSERT_EQ(problems.size(), 1u) << bad.dump();
        EXPECT_NE(problems[0].find("math.simdTier"), std::string::npos);
    }
}

TEST(MathSimdSettingTest, ApplyProjectSettingsValidatesItAndChangesNothingLive)
{
    EXPECT_TRUE(ApplyProjectSettings(json{{"math", {{"simdTier", "scalar"}}}}, std::set<std::string>{"math"}).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"math", {{"simdTier", "auto"}}}}, std::set<std::string>{"math"}).empty());

    const auto bad = ApplyProjectSettings(json{{"math", {{"simdTier", "banana"}}}}, std::set<std::string>{"math"});
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_NE(bad[0].find("math.simdTier"), std::string::npos);

    const auto notAnObject = ApplyProjectSettings(json{{"math", 5}}, std::set<std::string>{"math"});
    ASSERT_EQ(notAnObject.size(), 1u);
    EXPECT_NE(notAnObject[0].find("math"), std::string::npos);
}
