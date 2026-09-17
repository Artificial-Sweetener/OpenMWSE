#include <components/diagnostics/protocol.hpp>
#include <components/diagnostics/watersettings.hpp>

#include <gtest/gtest.h>

#include <limits>

TEST(DiagnosticWaterSettings, PreservesSliderDomainAndProtocolSupport)
{
    for (const float height : { 0.f, 1.f, 30.f, 128.f, 250.f })
    {
        const Diagnostics::WaterSettings settings{ height, height > 0.f, height >= 30.f, 0.75f };
        EXPECT_EQ(Diagnostics::deserializeWaterSettings(Diagnostics::serializeWaterSettings(settings)), settings);
    }
    EXPECT_TRUE(
        Diagnostics::isSupportedCommand(static_cast<std::uint16_t>(Diagnostics::CommandType::SetWaterSettings)));
}

TEST(DiagnosticWaterSettings, RejectsInvalidPayloadsBeforeMenuMutation)
{
    const auto valid = Diagnostics::serializeWaterSettings({});
    EXPECT_ANY_THROW(Diagnostics::deserializeWaterSettings("water-version: 1"));
    EXPECT_ANY_THROW(Diagnostics::deserializeWaterSettings(valid + "\nunknown: true"));
    EXPECT_ANY_THROW(Diagnostics::deserializeWaterSettings(std::string(4097, 'x')));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ -1.f }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 1.5f }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 251.f }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ std::numeric_limits<float>::quiet_NaN() }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 0.f, true, false, -1.f }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 0.f, true, false, 2.1f }));
}

/// Preserve version-one route fixtures while defaulting newly introduced optical controls safely.
TEST(DiagnosticWaterSettings, ReadsVersionOnePayloadWithCompatibleOpticalDefaults)
{
    const Diagnostics::WaterSettings expected{ 32.f, false, false, 1.f };
    EXPECT_EQ(Diagnostics::deserializeWaterSettings("water-version: 1\ngeometric-wave-height: 32"), expected);
}

/// Preserve version-two route fixtures while keeping reflected light dormant.
TEST(DiagnosticWaterSettings, ReadsVersionTwoPayloadWithCompatibleReflectedDefault)
{
    const Diagnostics::WaterSettings expected{ 32.f, true, false, 0.75f };
    EXPECT_EQ(Diagnostics::deserializeWaterSettings(
                  "water-version: 2\ngeometric-wave-height: 32\ncaustics: true\ncaustics-strength: 0.75"),
        expected);
}
