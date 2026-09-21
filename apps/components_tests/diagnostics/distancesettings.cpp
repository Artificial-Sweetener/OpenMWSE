#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/protocol.hpp>

#include <gtest/gtest.h>

#include <limits>

TEST(DiagnosticDistanceSettings, PreservesCanonicalValuesAndUnlimitedSentinels)
{
    for (bool cells : { false, true })
        for (float shadow : { -1.f, 0.f, 28672.f })
        {
            const Diagnostics::DistanceSettings settings{ 123456.5f, 28672.f, shadow, cells, std::nullopt,
                std::nullopt };
            EXPECT_EQ(
                Diagnostics::deserializeDistanceSettings(Diagnostics::serializeDistanceSettings(settings)), settings);
        }
    EXPECT_TRUE(
        Diagnostics::isSupportedCommand(static_cast<std::uint16_t>(Diagnostics::CommandType::SetDistanceSettings)));
}

TEST(DiagnosticDistanceSettings, RejectsInvalidPayloadsBeforeMutation)
{
    const auto valid = Diagnostics::serializeDistanceSettings({});
    EXPECT_ANY_THROW(Diagnostics::deserializeDistanceSettings("distance-version: 1"));
    EXPECT_ANY_THROW(Diagnostics::deserializeDistanceSettings(valid + "\nunknown: true"));
    EXPECT_ANY_THROW(Diagnostics::deserializeDistanceSettings(std::string(4097, 'x')));
    EXPECT_ANY_THROW(
        Diagnostics::deserializeDistanceSettings("distance-version: 1\nview-world-units: 0\ngrass-world-units: "
                                                 "1\nshadow-world-units: 1\nuse-cell-editor: true"));
    EXPECT_ANY_THROW(Diagnostics::serializeDistanceSettings({ 1, -1, 0, true, std::nullopt, std::nullopt }));
    EXPECT_ANY_THROW(Diagnostics::serializeDistanceSettings(
        { 1, 0, std::numeric_limits<float>::quiet_NaN(), true, std::nullopt, std::nullopt }));
}

TEST(DiagnosticDistanceSettings, VersionTwoRetainsResolutionAndRejectsInvalidDimensions)
{
    const Diagnostics::DistanceSettings settings{ 65536, 28672, 8192, true, 2304, std::nullopt };
    EXPECT_EQ(Diagnostics::deserializeDistanceSettings(Diagnostics::serializeDistanceSettings(settings)), settings);
    EXPECT_ANY_THROW(Diagnostics::serializeDistanceSettings({ 65536, 28672, 8192, true, 0, std::nullopt }));
    EXPECT_ANY_THROW(Diagnostics::serializeDistanceSettings({ 65536, 28672, 8192, true, 32768, std::nullopt }));
}

TEST(DiagnosticDistanceSettings, VersionThreeRetainsFogRangeAndIndependentLocks)
{
    const Diagnostics::DistanceSettings settings{ 32768, 8192, 16384, true, 4096,
        Diagnostics::FogDistanceSettings{ true, 8192, 16384, true, false } };
    EXPECT_EQ(Diagnostics::deserializeDistanceSettings(Diagnostics::serializeDistanceSettings(settings)), settings);
    EXPECT_ANY_THROW(Diagnostics::serializeDistanceSettings(
        { 32768, 8192, 16384, true, 4096, Diagnostics::FogDistanceSettings{ true, 16384, 8192, false, false } }));
    EXPECT_ANY_THROW(Diagnostics::serializeDistanceSettings(
        { 32768, 8192, 16384, true, std::nullopt, Diagnostics::FogDistanceSettings{} }));
}
