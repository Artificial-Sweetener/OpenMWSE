#include "atmospherescenario.hpp"

#include <cmath>
#include <stdexcept>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace Diagnostics
{
    namespace
    {
        /// Reject unknown mapping keys so misspelled diagnostic controls cannot silently change a capture.
        void requireKnownKeys(
            const YAML::Node& mapping, const std::unordered_set<std::string>& known, std::string_view description)
        {
            if (!mapping.IsMap())
                throw std::invalid_argument(std::string(description) + " must be a mapping");
            for (const auto& entry : mapping)
            {
                const std::string key = entry.first.as<std::string>();
                if (!known.contains(key))
                    throw std::invalid_argument(std::string(description) + " contains unknown key " + key);
            }
        }

        /// Validate fields shared by serialization and deserialization at the authoritative format boundary.
        void validateScenario(const AtmosphereScenario& scenario)
        {
            if (scenario.empty())
                throw std::invalid_argument("atmosphere scenario does not contain an override");
            if (scenario.mGameHour
                && (!std::isfinite(*scenario.mGameHour) || *scenario.mGameHour < 0.0 || *scenario.mGameHour >= 24.0))
                throw std::invalid_argument("atmosphere scenario game hour must be within [0, 24)");
            if (scenario.mGameHour && scenario.mStartGameHour)
                throw std::invalid_argument("atmosphere scenario cannot lock and advance game hour together");
            if (scenario.mStartGameHour
                && (!std::isfinite(*scenario.mStartGameHour) || *scenario.mStartGameHour < 0.0
                    || *scenario.mStartGameHour >= 24.0))
                throw std::invalid_argument("atmosphere scenario starting game hour must be within [0, 24)");
            if (scenario.mRecentPrecipitation
                && (!std::isfinite(*scenario.mRecentPrecipitation) || *scenario.mRecentPrecipitation < 0.f
                    || *scenario.mRecentPrecipitation > 1.f))
                throw std::invalid_argument("atmosphere scenario recent precipitation must be within [0, 1]");
            if (scenario.mEnvironment)
            {
                const DiagnosticEnvironmentState& environment = *scenario.mEnvironment;
                if (environment.mTraits.empty() && !environment.mTerrainHeightWorldUnits)
                    throw std::invalid_argument("atmosphere scenario environment does not contain an override");
                for (const auto& [trait, weight] : environment.mTraits)
                {
                    if (trait.empty() || !std::isfinite(weight) || weight < 0.f || weight > 1.f)
                        throw std::invalid_argument(
                            "atmosphere scenario environment traits require names and weights within [0, 1]");
                }
                if (environment.mTerrainHeightWorldUnits && !std::isfinite(*environment.mTerrainHeightWorldUnits))
                    throw std::invalid_argument("atmosphere scenario environment terrain height must be finite");
            }
            if (!scenario.mWeather)
                return;

            const DiagnosticWeatherState& weather = *scenario.mWeather;
            if (weather.mCurrent.empty())
                throw std::invalid_argument("atmosphere scenario current weather is empty");
            if (!std::isfinite(weather.mTransition) || weather.mTransition < 0.f || weather.mTransition > 1.f)
                throw std::invalid_argument("atmosphere scenario weather transition must be within [0, 1]");
            if (!weather.mNext && weather.mTransition != 0.f)
                throw std::invalid_argument("steady atmosphere scenario weather cannot have transition progress");
            if (weather.mNext && weather.mNext->empty())
                throw std::invalid_argument("atmosphere scenario next weather is empty");
        }
    }

    std::string serializeAtmosphereScenario(const AtmosphereScenario& scenario)
    {
        validateScenario(scenario);
        YAML::Emitter output;
        output << YAML::BeginMap << YAML::Key << "version" << YAML::Value << atmosphereScenarioVersion;
        if (scenario.mWeather)
        {
            output << YAML::Key << "weather" << YAML::Value << YAML::BeginMap << YAML::Key << "current" << YAML::Value
                   << scenario.mWeather->mCurrent;
            if (scenario.mWeather->mNext)
            {
                output << YAML::Key << "next" << YAML::Value << *scenario.mWeather->mNext << YAML::Key << "transition"
                       << YAML::Value << scenario.mWeather->mTransition;
            }
            output << YAML::EndMap;
        }
        if (scenario.mGameHour)
            output << YAML::Key << "game-hour" << YAML::Value << *scenario.mGameHour;
        if (scenario.mStartGameHour)
            output << YAML::Key << "start-game-hour" << YAML::Value << *scenario.mStartGameHour;
        if (scenario.mMedia)
        {
            output << YAML::Key << "media" << YAML::Value << YAML::BeginMap << YAML::Key << "main-clouds" << YAML::Value
                   << scenario.mMedia->mMainClouds << YAML::Key << "low-clouds" << YAML::Value
                   << scenario.mMedia->mLowClouds << YAML::Key << "mist" << YAML::Value << scenario.mMedia->mMist
                   << YAML::Key << "fog" << YAML::Value << scenario.mMedia->mFog << YAML::EndMap;
        }
        if (scenario.mDistanceFog)
            output << YAML::Key << "distance-fog" << YAML::Value << *scenario.mDistanceFog;
        if (scenario.mCloudSeed)
            output << YAML::Key << "cloud-seed" << YAML::Value << *scenario.mCloudSeed;
        if (scenario.mWeatherEffects)
        {
            output << YAML::Key << "weather-effects" << YAML::Value << YAML::BeginMap << YAML::Key << "precipitation"
                   << YAML::Value << scenario.mWeatherEffects->mPrecipitation << YAML::Key << "lightning" << YAML::Value
                   << scenario.mWeatherEffects->mLightning << YAML::EndMap;
        }
        if (scenario.mEnvironment)
        {
            output << YAML::Key << "environment" << YAML::Value << YAML::BeginMap;
            if (!scenario.mEnvironment->mTraits.empty())
            {
                output << YAML::Key << "traits" << YAML::Value << YAML::BeginMap;
                for (const auto& [trait, weight] : scenario.mEnvironment->mTraits)
                    output << YAML::Key << trait << YAML::Value << weight;
                output << YAML::EndMap;
            }
            if (scenario.mEnvironment->mTerrainHeightWorldUnits)
                output << YAML::Key << "terrain-height" << YAML::Value
                       << *scenario.mEnvironment->mTerrainHeightWorldUnits;
            output << YAML::EndMap;
        }
        if (scenario.mRecentPrecipitation)
            output << YAML::Key << "recent-precipitation" << YAML::Value << *scenario.mRecentPrecipitation;
        output << YAML::EndMap;
        if (!output.good())
            throw std::runtime_error("failed serializing atmosphere scenario");
        return std::string(output.c_str()) + '\n';
    }

    AtmosphereScenario deserializeAtmosphereScenario(std::string_view text)
    {
        const YAML::Node root = YAML::Load(std::string(text));
        requireKnownKeys(root,
            { "version", "weather", "game-hour", "media", "distance-fog", "cloud-seed", "weather-effects",
                "environment", "recent-precipitation", "start-game-hour" },
            "atmosphere scenario");
        if (!root["version"])
            throw std::invalid_argument("atmosphere scenario version is not supported");
        const unsigned version = root["version"].as<unsigned>();
        if (version < 1 || version > atmosphereScenarioVersion)
            throw std::invalid_argument("atmosphere scenario version is not supported");
        if (version < 2 && (root["media"] || root["distance-fog"]))
            throw std::invalid_argument("atmosphere scenario rendering controls require version 2");
        if (version < 3 && root["cloud-seed"])
            throw std::invalid_argument("atmosphere scenario cloud seed requires version 3");
        if (version < 4 && root["weather-effects"])
            throw std::invalid_argument("atmosphere scenario weather effects require version 4");
        if (version < 5 && root["environment"])
            throw std::invalid_argument("atmosphere scenario environment requires version 5");
        if (version < 6 && root["recent-precipitation"])
            throw std::invalid_argument("atmosphere scenario recent precipitation requires version 6");

        if (version < 7 && root["start-game-hour"])
            throw std::invalid_argument("atmosphere scenario starting game hour requires version 7");

        AtmosphereScenario scenario;
        if (const YAML::Node weather = root["weather"])
        {
            requireKnownKeys(weather, { "current", "next", "transition" }, "atmosphere scenario weather");
            if (!weather["current"])
                throw std::invalid_argument("atmosphere scenario weather requires current");
            DiagnosticWeatherState state;
            state.mCurrent = weather["current"].as<std::string>();
            if (weather["next"])
            {
                if (!weather["transition"])
                    throw std::invalid_argument("transitioning atmosphere scenario weather requires transition");
                state.mNext = weather["next"].as<std::string>();
                state.mTransition = weather["transition"].as<float>();
            }
            else if (weather["transition"])
                throw std::invalid_argument("atmosphere scenario transition requires next weather");
            scenario.mWeather = std::move(state);
        }
        if (root["game-hour"])
            scenario.mGameHour = root["game-hour"].as<double>();
        if (root["start-game-hour"])
            scenario.mStartGameHour = root["start-game-hour"].as<double>();
        if (const YAML::Node media = root["media"])
        {
            requireKnownKeys(media, { "main-clouds", "low-clouds", "mist", "fog" }, "atmosphere scenario media");
            for (const char* key : { "main-clouds", "low-clouds", "mist", "fog" })
            {
                if (!media[key])
                    throw std::invalid_argument(std::string("atmosphere scenario media requires ") + key);
            }
            scenario.mMedia = DiagnosticAtmosphereMedia{ media["main-clouds"].as<bool>(),
                media["low-clouds"].as<bool>(), media["mist"].as<bool>(), media["fog"].as<bool>() };
        }
        if (root["distance-fog"])
            scenario.mDistanceFog = root["distance-fog"].as<bool>();
        if (root["cloud-seed"])
            scenario.mCloudSeed = root["cloud-seed"].as<std::uint32_t>();
        if (const YAML::Node weatherEffects = root["weather-effects"])
        {
            requireKnownKeys(weatherEffects, { "precipitation", "lightning" }, "atmosphere scenario weather effects");
            for (const char* key : { "precipitation", "lightning" })
            {
                if (!weatherEffects[key])
                    throw std::invalid_argument(std::string("atmosphere scenario weather effects requires ") + key);
            }
            scenario.mWeatherEffects = DiagnosticWeatherEffects{ weatherEffects["precipitation"].as<bool>(),
                weatherEffects["lightning"].as<bool>() };
        }
        if (const YAML::Node environment = root["environment"])
        {
            requireKnownKeys(environment, { "traits", "terrain-height" }, "atmosphere scenario environment");
            DiagnosticEnvironmentState state;
            if (const YAML::Node traits = environment["traits"])
            {
                if (!traits.IsMap())
                    throw std::invalid_argument("atmosphere scenario environment traits must be a mapping");
                for (const auto& trait : traits)
                    state.mTraits.emplace(trait.first.as<std::string>(), trait.second.as<float>());
            }
            if (environment["terrain-height"])
                state.mTerrainHeightWorldUnits = environment["terrain-height"].as<float>();
            scenario.mEnvironment = std::move(state);
        }
        if (root["recent-precipitation"])
            scenario.mRecentPrecipitation = root["recent-precipitation"].as<float>();
        validateScenario(scenario);
        return scenario;
    }
}
