#include <components/diagnostics/protocol.hpp>
#include <components/diagnostics/watersettings.hpp>

#include <gtest/gtest.h>

#include <limits>

TEST(DiagnosticWaterSettings, PreservesSliderDomainAndProtocolSupport)
{
    for (const float height : { 0.f, 1.f, 30.f, 128.f, 250.f })
    {
        const Diagnostics::WaterSettings settings{ height, height > 0.f, 0.75f, 3, 4 };
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
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 0.f, true, -1.f }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 0.f, true, 2.1f }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 0.f, true, 1.f, -1, 0 }));
    EXPECT_ANY_THROW(Diagnostics::serializeWaterSettings({ 0.f, true, 1.f, 0, 5 }));
}

/// Preserve version-one route fixtures while defaulting newly introduced optical controls safely.
TEST(DiagnosticWaterSettings, ReadsVersionOnePayloadWithCompatibleOpticalDefaults)
{
    const Diagnostics::WaterSettings expected{ 32.f, false, 1.f, 2, 0 };
    EXPECT_EQ(Diagnostics::deserializeWaterSettings("water-version: 1\ngeometric-wave-height: 32"), expected);
}

/// Preserve version-two route fixtures while defaulting local-light budgets safely.
TEST(DiagnosticWaterSettings, ReadsVersionTwoPayloadWithCompatibleLocalLightDefaults)
{
    const Diagnostics::WaterSettings expected{ 32.f, true, 0.75f, 2, 0 };
    EXPECT_EQ(Diagnostics::deserializeWaterSettings(
                  "water-version: 2\ngeometric-wave-height: 32\ncaustics: true\ncaustics-strength: 0.75"),
        expected);
}

/// Accept and ignore the retired reflected-caustics field in version-three route fixtures.
TEST(DiagnosticWaterSettings, ReadsVersionThreePayloadWithoutRestoringReflectedCaustics)
{
    const Diagnostics::WaterSettings expected{ 32.f, true, 0.75f, 2, 0 };
    EXPECT_EQ(Diagnostics::deserializeWaterSettings("water-version: 3\ngeometric-wave-height: 32\ncaustics: true\n"
                                                    "reflected-caustics: true\ncaustics-strength: 0.75"),
        expected);
}

/// Accept version-four routes while discarding their retired reflected-caustics control.
TEST(DiagnosticWaterSettings, ReadsVersionFourPayloadWithoutRestoringReflectedCaustics)
{
    const Diagnostics::WaterSettings expected{ 32.f, true, 0.75f, 3, 4 };
    EXPECT_EQ(Diagnostics::deserializeWaterSettings("water-version: 4\ngeometric-wave-height: 32\ncaustics: true\n"
                                                    "reflected-caustics: true\ncaustics-strength: 0.75\n"
                                                    "interior-local-lights: 3\nexterior-local-lights: 4"),
        expected);
}
