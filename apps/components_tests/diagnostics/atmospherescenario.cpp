#include <components/diagnostics/atmospherescenario.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(DiagnosticAtmosphereScenario, RoundTripsSteadyAndTransitioningWeather)
    {
        const Diagnostics::AtmosphereScenario steady{ Diagnostics::DiagnosticWeatherState{ "Clear", std::nullopt, 0.f },
            18.25, Diagnostics::DiagnosticAtmosphereMedia{ true, false, false, false }, false, 927u,
            Diagnostics::DiagnosticWeatherEffects{ false, false },
            Diagnostics::DiagnosticEnvironmentState{ { { "wetland", 0.8f } }, 128.f }, 0.85f };
        EXPECT_EQ(Diagnostics::deserializeAtmosphereScenario(Diagnostics::serializeAtmosphereScenario(steady)), steady);

        const Diagnostics::AtmosphereScenario transitioning{ Diagnostics::DiagnosticWeatherState{
                                                                 "Cloudy", "Thunderstorm", 0.625f },
            6.5, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt };
        EXPECT_EQ(Diagnostics::deserializeAtmosphereScenario(Diagnostics::serializeAtmosphereScenario(transitioning)),
            transitioning);
    }

    TEST(DiagnosticAtmosphereScenario, RejectsAmbiguousOrOutOfRangeInputs)
    {
        EXPECT_THROW(
            Diagnostics::deserializeAtmosphereScenario("version: 1\nweather: { current: Clear, transition: 0.5 }\n"),
            std::invalid_argument);
        EXPECT_THROW(
            Diagnostics::deserializeAtmosphereScenario("version: 1\nweather: { current: Clear }\ngame-hour: 24\n"),
            std::invalid_argument);
        EXPECT_THROW(
            Diagnostics::deserializeAtmosphereScenario("version: 1\nweather: { current: Clear }\nweathre: Foggy\n"),
            std::invalid_argument);
        EXPECT_THROW(Diagnostics::deserializeAtmosphereScenario("version: 2\ncloud-seed: 7\n"), std::invalid_argument);
        EXPECT_THROW(Diagnostics::deserializeAtmosphereScenario(
                         "version: 3\nweather-effects: { precipitation: false, lightning: false }\n"),
            std::invalid_argument);
        EXPECT_THROW(
            Diagnostics::deserializeAtmosphereScenario("version: 4\nweather-effects: { precipitation: false }\n"),
            std::invalid_argument);
        EXPECT_THROW(
            Diagnostics::deserializeAtmosphereScenario("version: 4\nenvironment: { traits: { wetland: 1 } }\n"),
            std::invalid_argument);
        EXPECT_THROW(
            Diagnostics::deserializeAtmosphereScenario("version: 5\nenvironment: { traits: { wetland: 1.1 } }\n"),
            std::invalid_argument);
        EXPECT_THROW(Diagnostics::deserializeAtmosphereScenario("version: 5\nenvironment: { traits: {} }\n"),
            std::invalid_argument);
        EXPECT_THROW(Diagnostics::deserializeAtmosphereScenario("version: 5\nrecent-precipitation: 0.8\n"),
            std::invalid_argument);
        EXPECT_THROW(Diagnostics::deserializeAtmosphereScenario("version: 6\nrecent-precipitation: 1.1\n"),
            std::invalid_argument);
    }

    TEST(DiagnosticAtmosphereScenario, RequiresAtLeastOneOverride)
    {
        EXPECT_THROW(Diagnostics::deserializeAtmosphereScenario("version: 1\n"), std::invalid_argument);
    }

    /// Verify that existing version-one capsules retain their weather and time semantics.
    TEST(DiagnosticAtmosphereScenario, ReadsVersionOneScenariosWithoutMediaOverrides)
    {
        const Diagnostics::AtmosphereScenario scenario = Diagnostics::deserializeAtmosphereScenario(
            "version: 1\nweather: { current: Thunderstorm }\ngame-hour: 13\n");
        ASSERT_TRUE(scenario.mWeather);
        EXPECT_EQ(scenario.mWeather->mCurrent, "Thunderstorm");
        EXPECT_DOUBLE_EQ(*scenario.mGameHour, 13.0);
        EXPECT_FALSE(scenario.mMedia);
        EXPECT_FALSE(scenario.mDistanceFog);
        EXPECT_FALSE(scenario.mCloudSeed);
        EXPECT_FALSE(scenario.mWeatherEffects);
        EXPECT_FALSE(scenario.mEnvironment);
        EXPECT_FALSE(scenario.mRecentPrecipitation);
    }
}
