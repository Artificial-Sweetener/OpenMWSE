#include <apps/openmw/diagnosticworkcounter.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>

TEST(DiagnosticWorkCounterTest, DefinesBoundedUniqueStableNames)
{
    const std::span counters = OMW::diagnosticWorkCounters();
    ASSERT_FALSE(counters.empty());
    ASSERT_LE(counters.size(), 64);

    std::set<std::string> names;
    for (const OMW::DiagnosticWorkCounter& counter : counters)
    {
        EXPECT_FALSE(counter.mName.empty());
        EXPECT_FALSE(counter.mStatistic.empty());
        EXPECT_TRUE(names.emplace(counter.mName).second) << counter.mName;
    }
}
