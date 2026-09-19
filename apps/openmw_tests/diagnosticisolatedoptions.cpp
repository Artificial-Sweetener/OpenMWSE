#include <apps/openmw-diagnosticctl/isolatedoptions.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

TEST(DiagnosticIsolatedOptionsTest, UsesThreeMinuteStartupTimeoutByDefault)
{
    EXPECT_EQ(DiagnosticCtl::parseStartupTimeout(std::nullopt), std::chrono::minutes(3));
}

TEST(DiagnosticIsolatedOptionsTest, AcceptsLargeBoundedProfileStartupTimeout)
{
    EXPECT_EQ(DiagnosticCtl::parseStartupTimeout(std::string("900")), std::chrono::minutes(15));
    EXPECT_EQ(DiagnosticCtl::parseStartupTimeout(std::string("3600")), std::chrono::hours(1));
}

TEST(DiagnosticIsolatedOptionsTest, RejectsInvalidProfileStartupTimeout)
{
    EXPECT_THROW(DiagnosticCtl::parseStartupTimeout(std::string("0")), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::parseStartupTimeout(std::string("3601")), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::parseStartupTimeout(std::string("30seconds")), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::parseStartupTimeout(std::string("-1")), std::invalid_argument);
}

TEST(DiagnosticIsolatedOptionsTest, StartsFromExactlyOneNonInteractiveWorldSource)
{
    std::vector<std::string> savedWorld{ "openmw" };
    DiagnosticCtl::addWorldStartArguments(savedWorld, std::filesystem::path("world.omwsave"), false);
    EXPECT_EQ(savedWorld, (std::vector<std::string>{ "openmw", "--load-savegame", "world.omwsave", "--skip-menu" }));

    std::vector<std::string> freshWorld{ "openmw" };
    DiagnosticCtl::addWorldStartArguments(freshWorld, std::nullopt, true);
    EXPECT_EQ(freshWorld, (std::vector<std::string>{ "openmw", "--skip-menu", "--new-game" }));

    std::vector<std::string> menu{ "openmw" };
    DiagnosticCtl::addWorldStartArguments(menu, std::nullopt, false);
    EXPECT_EQ(menu, (std::vector<std::string>{ "openmw" }));

    EXPECT_THROW(DiagnosticCtl::addWorldStartArguments(menu, std::filesystem::path("world.omwsave"), true),
        std::invalid_argument);
}

/// Forward only explicitly requested diagnostic presentation state to the child process.
TEST(DiagnosticIsolatedOptionsTest, ForwardsProductionStatsOverlayIndependentlyFromWindowVisibility)
{
    std::vector<std::string> arguments;
    DiagnosticCtl::addDiagnosticPresentationArguments(arguments, false, true);
    EXPECT_EQ(arguments, (std::vector<std::string>{ "--diagnostic-stats-overlay" }));

    arguments.clear();
    DiagnosticCtl::addDiagnosticPresentationArguments(arguments, true, false);
    EXPECT_EQ(arguments, (std::vector<std::string>{ "--diagnostic-headed" }));

    arguments.clear();
    DiagnosticCtl::addDiagnosticPresentationArguments(arguments, true, true);
    EXPECT_EQ(arguments, (std::vector<std::string>{ "--diagnostic-headed", "--diagnostic-stats-overlay" }));
}
