#include <gtest/gtest.h>

#include <math/Functions.hpp>

using namespace N2Engine::Math;

TEST(FunctionsTest, MoveTowardsStepsByMaxDelta)
{
    // It had no return statement, so every call returned an undefined value
    EXPECT_FLOAT_EQ(Functions::MoveTowards(0.0f, 10.0f, 3.0f), 3.0f);
    EXPECT_FLOAT_EQ(Functions::MoveTowards(10.0f, 0.0f, 3.0f), 7.0f);
    EXPECT_FLOAT_EQ(Functions::MoveTowards(-2.0f, -5.0f, 1.0f), -3.0f);
}

TEST(FunctionsTest, MoveTowardsStopsAtTheTarget)
{
    EXPECT_FLOAT_EQ(Functions::MoveTowards(0.0f, 2.0f, 5.0f), 2.0f);
    EXPECT_FLOAT_EQ(Functions::MoveTowards(4.0f, 1.0f, 3.0f), 1.0f); // exactly maxDelta away
    EXPECT_FLOAT_EQ(Functions::MoveTowards(1.5f, 1.5f, 0.0f), 1.5f);
}
