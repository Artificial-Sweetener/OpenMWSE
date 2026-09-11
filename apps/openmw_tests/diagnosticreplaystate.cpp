#include <apps/openmw-diagnosticctl/replaystate.hpp>

#include <gtest/gtest.h>
#include <stdexcept>

/// Compare the old recorded trajectory after the regional color state was added to the palette protocol.
TEST(DiagnosticReplayStateTest, AcceptsOnlyVersionedRegionalHistoryAddition)
{
    constexpr std::string_view old = "{clock: 4, palette: {runtime-version: 1, applied: [1, 2, 3]}}";
    constexpr std::string_view current
        = "{clock: 4, palette: {runtime-version: 2, regional-colors: {target: default}, applied: [1, 2, 3]}}";
    EXPECT_NO_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(old, current));
    EXPECT_NO_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(current, current));
    EXPECT_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(current, old), std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(
                     old, "{clock: 4, palette: {runtime-version: 2, applied: [1, 2, 3]}}"),
        std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(
                     old, "{clock: 4, palette: {runtime-version: 2, regional-colors: {}, applied: [1, 2, 4]}}"),
        std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(
                     old, "{clock: 5, palette: {runtime-version: 2, regional-colors: {}, applied: [1, 2, 3]}}"),
        std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(
                     old, "{clock: 4, palette: {runtime-version: 3, regional-colors: {}, applied: [1, 2, 3]}}"),
        std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateAtmosphereRuntimeParity(old,
                     "{clock: 4, palette: {runtime-version: 2, regional-colors: {}, applied: [1, 2, 3], extra: 1}}"),
        std::runtime_error);
    EXPECT_THROW(
        DiagnosticCtl::validateAtmosphereRuntimeParity(current,
            "{clock: 4, palette: {runtime-version: 2, regional-colors: {target: changed}, applied: [1, 2, 3]}}"),
        std::runtime_error);
}
