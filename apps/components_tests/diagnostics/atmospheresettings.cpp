#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/protocol.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(DiagnosticAtmosphereSettings, RoundTripsIndependentChoicesAtEveryQuality)
    {
        for (bool scattering : { false, true })
            for (bool clouds : { false, true })
                for (int quality = 0; quality <= 3; ++quality)
                {
                    const Diagnostics::AtmosphereSettings settings{ scattering, clouds, quality, false, true };
                    EXPECT_EQ(
                        Diagnostics::deserializeAtmosphereSettings(Diagnostics::serializeAtmosphereSettings(settings)),
                        settings);
                }
        EXPECT_TRUE(Diagnostics::isSupportedCommand(
            static_cast<std::uint16_t>(Diagnostics::CommandType::SetAtmosphereSettings)));
    }

    TEST(DiagnosticAtmosphereSettings, RejectsIncompleteUnknownAndOutOfRangeUpdates)
    {
        const std::string valid = Diagnostics::serializeAtmosphereSettings({});
        EXPECT_ANY_THROW(Diagnostics::deserializeAtmosphereSettings("settings-version: 1"));
        EXPECT_ANY_THROW(Diagnostics::deserializeAtmosphereSettings(valid + "\nunknown: true"));
        EXPECT_ANY_THROW(Diagnostics::deserializeAtmosphereSettings(
            "settings-version: 2\nscattering: true\nclouds: true\nquality: 2\nshadows: true\nreflections: true"));
        EXPECT_ANY_THROW(Diagnostics::deserializeAtmosphereSettings(
            "settings-version: 1\nscattering: true\nclouds: true\nquality: 4\nshadows: true\nreflections: true"));
        EXPECT_ANY_THROW(Diagnostics::deserializeAtmosphereSettings(std::string(4097, 'x')));
    }

    /// Older controllers retain their five preferences and receive the current defaults for additive options.
    TEST(DiagnosticAtmosphereSettings, ReadsVersionOneAndRoundTripsVersionTwoSunlightOptions)
    {
        const auto legacy = Diagnostics::deserializeAtmosphereSettings(
            "settings-version: 1\nscattering: false\nclouds: false\nquality: 1\nshadows: true\nreflections: false");
        EXPECT_FALSE(legacy.mScattering);
        EXPECT_FALSE(legacy.mClouds);
        EXPECT_EQ(legacy.mQuality, 1);
        EXPECT_EQ(legacy.mAppearance, 0);
        EXPECT_TRUE(legacy.mGodrays);
        EXPECT_FALSE(legacy.mSunGlare);
        EXPECT_TRUE(legacy.mCloudSunOcclusion);
        for (int appearance : { 0, 1 })
            for (bool rays : { false, true })
                for (bool glare : { false, true })
                    for (bool occlusion : { false, true })
                    {
                        auto options = legacy;
                        options.mAppearance = appearance;
                        options.mGodrays = rays;
                        options.mSunGlare = glare;
                        options.mCloudSunOcclusion = occlusion;
                        EXPECT_EQ(Diagnostics::deserializeAtmosphereSettings(
                                      Diagnostics::serializeAtmosphereSettings(options)),
                            options);
                    }
        auto invalid = legacy;
        invalid.mAppearance = 2;
        EXPECT_ANY_THROW(Diagnostics::serializeAtmosphereSettings(invalid));
    }
}
