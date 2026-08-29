#include <apps/openmw/diagnosticstartup.hpp>

#include <components/diagnostics/localtransport.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{
    using namespace std::string_view_literals;

    TEST(DiagnosticStartupTest, UsesBuildDefaultWithoutLaunchOverrides)
    {
        EXPECT_EQ(OMW::resolveDiagnosticEndpoint({}, std::nullopt, true), Diagnostics::defaultLocalEndpoint());
        EXPECT_TRUE(OMW::resolveDiagnosticEndpoint({}, std::nullopt, false).empty());
    }

    TEST(DiagnosticStartupTest, EnvironmentOverridesBuildDefault)
    {
        EXPECT_EQ(OMW::resolveDiagnosticEndpoint({}, "1"sv, false), Diagnostics::defaultLocalEndpoint());
        EXPECT_TRUE(OMW::resolveDiagnosticEndpoint({}, "0"sv, true).empty());
    }

    TEST(DiagnosticStartupTest, ExplicitChoiceOverridesEnvironmentAndBuildDefault)
    {
        EXPECT_EQ(OMW::resolveDiagnosticEndpoint("default", "0"sv, false), Diagnostics::defaultLocalEndpoint());
        EXPECT_EQ(OMW::resolveDiagnosticEndpoint("protected-endpoint", "0"sv, false), "protected-endpoint");
        EXPECT_TRUE(OMW::resolveDiagnosticEndpoint("off", "1"sv, true).empty());
    }

    TEST(DiagnosticStartupTest, RejectsInvalidEnvironmentOverride)
    {
        EXPECT_THROW(OMW::resolveDiagnosticEndpoint({}, ""sv, true), std::invalid_argument);
        EXPECT_THROW(OMW::resolveDiagnosticEndpoint({}, "enabled"sv, false), std::invalid_argument);
    }
}
