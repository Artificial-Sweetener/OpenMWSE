#include "atmospherepalette.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include <osg/Vec4f>
#include <yaml-cpp/yaml.h>

#include <components/vfs/manager.hpp>

#include "atmosphereenvironment.hpp"
#include "weather.hpp"

namespace MWWorld
{
    namespace
    {
        constexpr float sRegionTransitionSeconds = 20.f;
        constexpr std::array<std::string_view, 10> sWeatherNames = { "Clear", "Cloudy", "Foggy", "Overcast", "Rain",
            "Thunderstorm", "Ashstorm", "Blight", "Snow", "Blizzard" };

        std::string lowerCase(std::string_view value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return result;
        }

        /// Read a finite three-channel atmosphere value with field-qualified errors.
        osg::Vec3f readVec3(const YAML::Node& node, std::string_view path)
        {
            if (!node.IsSequence() || node.size() != 3)
                throw std::runtime_error("OpenMWSE atmosphere value must contain three numbers: " + std::string(path));
            const osg::Vec3f value(node[0].as<float>(), node[1].as<float>(), node[2].as<float>());
            if (!std::isfinite(value.x()) || !std::isfinite(value.y()) || !std::isfinite(value.z()))
                throw std::runtime_error("OpenMWSE atmosphere value must be finite: " + std::string(path));
            return value;
        }

        /// Read a finite atmosphere scalar constrained to its supported physical range.
        float readBoundedFloat(
            const YAML::Node& source, std::string_view name, std::string_view path, float minimum, float maximum)
        {
            const float value = source[std::string(name)].as<float>();
            if (!std::isfinite(value) || value < minimum || value > maximum)
                throw std::runtime_error("OpenMWSE atmosphere value is outside its supported range: "
                    + std::string(path) + "." + std::string(name));
            return value;
        }

        osg::Vec4f readColor(const YAML::Node& node, std::string_view path)
        {
            const osg::Vec3f color = readVec3(node, path);
            return { color.x(), color.y(), color.z(), 1.f };
        }

        struct ColorCycle
        {
            osg::Vec4f mSunrise;
            osg::Vec4f mDay;
            osg::Vec4f mSunset;
            osg::Vec4f mNight;
        };

        ColorCycle readCycle(const YAML::Node& weather, std::string_view prefix, std::string_view path)
        {
            const std::string base(prefix);
            return { readColor(weather[base + "SunriseColor"], std::string(path) + "." + base + "SunriseColor"),
                readColor(weather[base + "DayColor"], std::string(path) + "." + base + "DayColor"),
                readColor(weather[base + "SunsetColor"], std::string(path) + "." + base + "SunsetColor"),
                readColor(weather[base + "NightColor"], std::string(path) + "." + base + "NightColor") };
        }

        ColorCycle blend(const ColorCycle& from, const ColorCycle& to, float factor)
        {
            return { from.mSunrise * (1.f - factor) + to.mSunrise * factor,
                from.mDay * (1.f - factor) + to.mDay * factor, from.mSunset * (1.f - factor) + to.mSunset * factor,
                from.mNight * (1.f - factor) + to.mNight * factor };
        }

        struct WeatherColors
        {
            ColorCycle mSky;
            ColorCycle mFog;
            ColorCycle mAmbient;
            ColorCycle mSun;
            osg::Vec4f mSunDiscSunset;
        };

        WeatherColors blend(const WeatherColors& from, const WeatherColors& to, float factor)
        {
            return { blend(from.mSky, to.mSky, factor), blend(from.mFog, to.mFog, factor),
                blend(from.mAmbient, to.mAmbient, factor), blend(from.mSun, to.mSun, factor),
                from.mSunDiscSunset * (1.f - factor) + to.mSunDiscSunset * factor };
        }

        struct Palette
        {
            std::array<WeatherColors, sWeatherNames.size()> mWeather;
            osg::Vec3f mInScattering;
            osg::Vec3f mOutScattering;
        };

        /// Modulate weather-owned volumes with smoothly blended local environment evidence.
        struct VolumeModifiers
        {
            float mCloudDensityMultiplier = 1.f;
            float mCloudAltitudeOffsetWorldUnits = 0.f;
            float mCloudThicknessMultiplier = 1.f;
            float mLowCloudOccurrenceMultiplier = 1.f;
            float mLowCloudDensityMultiplier = 1.f;
            float mLowCloudAltitudeOffsetWorldUnits = 0.f;
            float mLowCloudThicknessMultiplier = 1.f;
            float mMistDensityMultiplier = 1.f;
            float mMistAltitudeOffsetWorldUnits = 0.f;
            float mMistThicknessMultiplier = 1.f;
        };

        VolumeModifiers blend(const VolumeModifiers& from, const VolumeModifiers& to, float factor)
        {
            return { std::lerp(from.mCloudDensityMultiplier, to.mCloudDensityMultiplier, factor),
                std::lerp(from.mCloudAltitudeOffsetWorldUnits, to.mCloudAltitudeOffsetWorldUnits, factor),
                std::lerp(from.mCloudThicknessMultiplier, to.mCloudThicknessMultiplier, factor),
                std::lerp(from.mLowCloudOccurrenceMultiplier, to.mLowCloudOccurrenceMultiplier, factor),
                std::lerp(from.mLowCloudDensityMultiplier, to.mLowCloudDensityMultiplier, factor),
                std::lerp(from.mLowCloudAltitudeOffsetWorldUnits, to.mLowCloudAltitudeOffsetWorldUnits, factor),
                std::lerp(from.mLowCloudThicknessMultiplier, to.mLowCloudThicknessMultiplier, factor),
                std::lerp(from.mMistDensityMultiplier, to.mMistDensityMultiplier, factor),
                std::lerp(from.mMistAltitudeOffsetWorldUnits, to.mMistAltitudeOffsetWorldUnits, factor),
                std::lerp(from.mMistThicknessMultiplier, to.mMistThicknessMultiplier, factor) };
        }

        /// Bind one semantic environment trait to its palette and medium modifiers.
        struct EnvironmentProfile
        {
            std::string mPalette;
            VolumeModifiers mVolumes;
        };

        /// Store validated per-weather cloud, mist, and shaft controls in world units.
        struct VolumeProfile
        {
            float mCloudDeckCoverage;
            float mCloudDensity;
            float mCloudBaseAltitudeWorldUnits;
            float mCloudThicknessWorldUnits;
            float mCloudType;
            float mCloudFeatureScale;
            float mCloudVerticalDevelopment;
            float mCloudAnvilStrength;
            float mCloudDeckContinuity;
            float mCloudConvectiveFrequency;
            float mCloudConvectiveMaturity;
            float mCloudStormOrganization;
            float mCloudTowerTaper;
            float mCloudBaseVariation;
            float mCloudTerrainFollowing;
            float mCloudTerrainClearanceWorldUnits;
            float mLowCloudCoverage;
            float mLowCloudDensity;
            float mLowCloudBaseAltitudeWorldUnits;
            float mLowCloudThicknessWorldUnits;
            float mLowCloudType;
            float mLowCloudFeatureScale;
            float mLowCloudVerticalDevelopment;
            float mLowCloudEnvironmentInfluence;
            float mMistDensity;
            float mMistBaseAltitudeWorldUnits;
            float mMistThicknessWorldUnits;
            float mShaftIntensity;
            float mFogDensity;
            float mFogBaseAltitudeWorldUnits;
            float mFogThicknessWorldUnits;
            float mFogAnisotropy;
        };

        /// Parse and validate one weather volume profile with field-qualified errors.
        VolumeProfile readVolumeProfile(const YAML::Node& source, std::string_view path)
        {
            if (!source.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere data is missing volume profile: " + std::string(path));
            return { readBoundedFloat(source, "cloudDeckCoverage", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudDensity", path, 0.f, 4.f),
                readBoundedFloat(source, "cloudAltitude", path, -10000.f, 200000.f),
                readBoundedFloat(source, "cloudThickness", path, 1.f, 200000.f),
                readBoundedFloat(source, "cloudType", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudFeatureScale", path, 0.25f, 4.f),
                readBoundedFloat(source, "cloudVerticalDevelopment", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudAnvilStrength", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudDeckContinuity", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudConvectiveFrequency", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudConvectiveMaturity", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudStormOrganization", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudTowerTaper", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudBaseVariation", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudTerrainFollowing", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudTerrainClearance", path, -20000.f, 100000.f),
                readBoundedFloat(source, "lowCloudCoverage", path, 0.f, 1.f),
                readBoundedFloat(source, "lowCloudDensity", path, 0.f, 4.f),
                readBoundedFloat(source, "lowCloudAltitude", path, -10000.f, 50000.f),
                readBoundedFloat(source, "lowCloudThickness", path, 1.f, 100000.f),
                readBoundedFloat(source, "lowCloudType", path, 0.f, 1.f),
                readBoundedFloat(source, "lowCloudFeatureScale", path, 0.25f, 4.f),
                readBoundedFloat(source, "lowCloudVerticalDevelopment", path, 0.f, 1.f),
                readBoundedFloat(source, "lowCloudEnvironmentInfluence", path, 0.f, 1.f),
                readBoundedFloat(source, "mistDensity", path, 0.f, 4.f),
                readBoundedFloat(source, "mistAltitude", path, -10000.f, 50000.f),
                readBoundedFloat(source, "mistThickness", path, 1.f, 100000.f),
                readBoundedFloat(source, "shaftIntensity", path, 0.f, 4.f),
                readBoundedFloat(source, "fogDensity", path, 0.f, 4.f),
                readBoundedFloat(source, "fogAltitude", path, -10000.f, 50000.f),
                readBoundedFloat(source, "fogThickness", path, 1.f, 100000.f),
                readBoundedFloat(source, "fogAnisotropy", path, 0.f, 0.95f) };
        }

        /// Return a Hermite transition across a normalized subrange.
        float smoothRange(float lower, float upper, float value)
        {
            const float normalized = std::clamp((value - lower) / std::max(upper - lower, 0.0001f), 0.f, 1.f);
            return normalized * normalized * (3.f - 2.f * normalized);
        }

        /// Stage visible deck, convection, and remnants without changing authoritative weather timing.
        WeatherPresentationState makeWeatherPresentationState(
            const VolumeProfile& from, const VolumeProfile& to, float transition)
        {
            const float fromSeverity
                = std::max({ from.mCloudDeckCoverage, from.mCloudStormOrganization, from.mCloudConvectiveMaturity });
            const float toSeverity
                = std::max({ to.mCloudDeckCoverage, to.mCloudStormOrganization, to.mCloudConvectiveMaturity });
            if (toSeverity > fromSeverity + 0.01f)
            {
                return { smoothRange(0.f, 0.72f, transition), smoothRange(0.18f, 1.f, transition),
                    smoothRange(0.08f, 0.92f, transition) };
            }
            if (toSeverity + 0.01f < fromSeverity)
            {
                return { smoothRange(0.18f, 1.f, transition), smoothRange(0.f, 0.62f, transition),
                    smoothRange(0.35f, 1.f, transition) };
            }
            const float blend = smoothRange(0.f, 1.f, transition);
            return { blend, blend, blend };
        }

        /// Interpolate weather volumes using independently staged presentation channels.
        VolumeProfile blend(
            const VolumeProfile& from, const VolumeProfile& to, const WeatherPresentationState& presentation)
        {
            const float deck = presentation.mDeckBlend;
            const float convection = presentation.mConvectionBlend;
            const float remnant = presentation.mRemnantBlend;
            return { std::lerp(from.mCloudDeckCoverage, to.mCloudDeckCoverage, deck),
                std::lerp(from.mCloudDensity, to.mCloudDensity, deck),
                std::lerp(from.mCloudBaseAltitudeWorldUnits, to.mCloudBaseAltitudeWorldUnits, deck),
                std::lerp(from.mCloudThicknessWorldUnits, to.mCloudThicknessWorldUnits, convection),
                std::lerp(from.mCloudType, to.mCloudType, convection),
                std::lerp(from.mCloudFeatureScale, to.mCloudFeatureScale, convection),
                std::lerp(from.mCloudVerticalDevelopment, to.mCloudVerticalDevelopment, convection),
                std::lerp(from.mCloudAnvilStrength, to.mCloudAnvilStrength, convection),
                std::lerp(from.mCloudDeckContinuity, to.mCloudDeckContinuity, deck),
                std::lerp(from.mCloudConvectiveFrequency, to.mCloudConvectiveFrequency, convection),
                std::lerp(from.mCloudConvectiveMaturity, to.mCloudConvectiveMaturity, convection),
                std::lerp(from.mCloudStormOrganization, to.mCloudStormOrganization, convection),
                std::lerp(from.mCloudTowerTaper, to.mCloudTowerTaper, convection),
                std::lerp(from.mCloudBaseVariation, to.mCloudBaseVariation, deck),
                std::lerp(from.mCloudTerrainFollowing, to.mCloudTerrainFollowing, remnant),
                std::lerp(from.mCloudTerrainClearanceWorldUnits, to.mCloudTerrainClearanceWorldUnits, remnant),
                std::lerp(from.mLowCloudCoverage, to.mLowCloudCoverage, remnant),
                std::lerp(from.mLowCloudDensity, to.mLowCloudDensity, remnant),
                std::lerp(from.mLowCloudBaseAltitudeWorldUnits, to.mLowCloudBaseAltitudeWorldUnits, remnant),
                std::lerp(from.mLowCloudThicknessWorldUnits, to.mLowCloudThicknessWorldUnits, remnant),
                std::lerp(from.mLowCloudType, to.mLowCloudType, remnant),
                std::lerp(from.mLowCloudFeatureScale, to.mLowCloudFeatureScale, remnant),
                std::lerp(from.mLowCloudVerticalDevelopment, to.mLowCloudVerticalDevelopment, remnant),
                std::lerp(from.mLowCloudEnvironmentInfluence, to.mLowCloudEnvironmentInfluence, remnant),
                std::lerp(from.mMistDensity, to.mMistDensity, remnant),
                std::lerp(from.mMistBaseAltitudeWorldUnits, to.mMistBaseAltitudeWorldUnits, remnant),
                std::lerp(from.mMistThicknessWorldUnits, to.mMistThicknessWorldUnits, remnant),
                std::lerp(from.mShaftIntensity, to.mShaftIntensity, deck),
                std::lerp(from.mFogDensity, to.mFogDensity, remnant),
                std::lerp(from.mFogBaseAltitudeWorldUnits, to.mFogBaseAltitudeWorldUnits, remnant),
                std::lerp(from.mFogThicknessWorldUnits, to.mFogThicknessWorldUnits, remnant),
                std::lerp(from.mFogAnisotropy, to.mFogAnisotropy, remnant) };
        }

        /// Resolve a canonical weather name to its volume-profile index.
        std::size_t weatherIndex(std::string_view weather)
        {
            const std::string normalized = lowerCase(weather);
            const auto found = std::find_if(sWeatherNames.begin(), sWeatherNames.end(),
                [&](std::string_view candidate) { return lowerCase(candidate) == normalized; });
            if (found == sWeatherNames.end())
                throw std::runtime_error("OpenMWSE atmosphere received unknown weather: " + std::string(weather));
            return static_cast<std::size_t>(std::distance(sWeatherNames.begin(), found));
        }

        Palette blend(const Palette& from, const Palette& to, float factor)
        {
            Palette result;
            for (std::size_t i = 0; i < result.mWeather.size(); ++i)
                result.mWeather[i] = blend(from.mWeather[i], to.mWeather[i], factor);
            result.mInScattering = from.mInScattering * (1.f - factor) + to.mInScattering * factor;
            result.mOutScattering = from.mOutScattering * (1.f - factor) + to.mOutScattering * factor;
            return result;
        }

        void setCycle(TimeOfDayInterpolator<osg::Vec4f>& destination, const ColorCycle& source)
        {
            destination.setSunriseValue(source.mSunrise);
            destination.setDayValue(source.mDay);
            destination.setSunsetValue(source.mSunset);
            destination.setNightValue(source.mNight);
        }

        bool usesScattering(std::string_view weather)
        {
            const std::string normalized = lowerCase(weather);
            return normalized == "clear" || normalized == "cloudy";
        }
    }

    /// Own parsed regional palettes, weather volumes, and their current transition state.
    struct AtmospherePalette::Impl
    {
        /// Parse and validate canonical palette and volume streams.
        Impl(std::istream& paletteStream, std::istream& volumeStream)
        {
            const YAML::Node root = YAML::Load(paletteStream);
            const YAML::Node presets = root["presets"];
            const YAML::Node environments = root["environments"];
            if (!presets.IsMap() || !environments.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere data must define presets and environments");

            const YAML::Node volumes = YAML::Load(volumeStream);
            if (!volumes.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere volume data must define weather profiles");

            for (std::size_t i = 0; i < sWeatherNames.size(); ++i)
            {
                const std::string weatherName(sWeatherNames[i]);
                mVolumes[i] = readVolumeProfile(volumes[weatherName], "atmosphere-volumes." + weatherName);
            }

            for (const auto& entry : presets)
            {
                const std::string name = entry.first.as<std::string>();
                const YAML::Node source = entry.second;
                Palette palette;
                palette.mInScattering = readVec3(source["inscatter"], "presets." + name + ".inscatter");
                palette.mOutScattering = readVec3(source["outscatter"], "presets." + name + ".outscatter");

                for (std::size_t i = 0; i < sWeatherNames.size(); ++i)
                {
                    const std::string weatherName(sWeatherNames[i]);
                    const YAML::Node weather = source[weatherName];
                    if (!weather.IsMap())
                        throw std::runtime_error(
                            "OpenMWSE atmosphere preset is missing weather: " + name + "." + weatherName);
                    const std::string path = "presets." + name + "." + weatherName;
                    palette.mWeather[i] = { readCycle(weather, "sky", path), readCycle(weather, "fog", path),
                        readCycle(weather, "ambient", path), readCycle(weather, "sun", path),
                        readColor(weather["sundiscSunsetColor"], path + ".sundiscSunsetColor") };
                }
                mPresets.emplace(name, std::move(palette));
            }

            const auto defaultPreset = mPresets.find("default");
            if (defaultPreset == mPresets.end())
                throw std::runtime_error("OpenMWSE atmosphere data is missing the default preset");

            for (const auto& entry : environments)
            {
                const std::string trait = lowerCase(entry.first.as<std::string>());
                const YAML::Node source = entry.second;
                if (!source.IsMap())
                    throw std::runtime_error("OpenMWSE atmosphere environment profile must be a map: " + trait);
                const std::string preset = source["palette"].as<std::string>();
                if (!mPresets.contains(preset))
                    throw std::runtime_error("OpenMWSE atmosphere environment references an unknown preset: " + preset);
                const std::string path = "environments." + trait;
                EnvironmentProfile profile;
                profile.mPalette = preset;
                profile.mVolumes = { readBoundedFloat(source, "cloudDensityMultiplier", path, 0.f, 4.f),
                    readBoundedFloat(source, "cloudAltitudeOffset", path, -100000.f, 100000.f),
                    readBoundedFloat(source, "cloudThicknessMultiplier", path, 0.1f, 4.f),
                    readBoundedFloat(source, "lowCloudOccurrenceMultiplier", path, 0.f, 4.f),
                    readBoundedFloat(source, "lowCloudDensityMultiplier", path, 0.f, 4.f),
                    readBoundedFloat(source, "lowCloudAltitudeOffset", path, -20000.f, 20000.f),
                    readBoundedFloat(source, "lowCloudThicknessMultiplier", path, 0.1f, 4.f),
                    readBoundedFloat(source, "mistDensityMultiplier", path, 0.f, 4.f),
                    readBoundedFloat(source, "mistAltitudeOffset", path, -20000.f, 20000.f),
                    readBoundedFloat(source, "mistThicknessMultiplier", path, 0.1f, 4.f) };
                mEnvironments.emplace(trait, std::move(profile));
            }

            mApplied = mTarget = defaultPreset->second;
        }

        void setEnvironment(const AtmosphereEnvironmentState& environment)
        {
            mTargetTerrainHeightWorldUnits = environment.mTerrainHeightWorldUnits;
            mTargetTerrainSampleCenterWorldUnits = environment.mTerrainSampleCenterWorldUnits;
            mTargetTerrainGradient = environment.mTerrainGradient;
            mTargetValleyDepthWorldUnits = environment.mValleyDepthWorldUnits;
            float totalEnvironmentWeight = 0.f;
            for (const auto& [trait, profile] : mEnvironments)
                totalEnvironmentWeight += environment.get(trait);
            const float normalization = totalEnvironmentWeight > 1.f ? 1.f / totalEnvironmentWeight : 1.f;
            float accumulatedWeight = std::max(0.f, 1.f - totalEnvironmentWeight * normalization);
            mTarget = mPresets.at("default");
            mTargetVolumes = {};

            for (const auto& [trait, profile] : mEnvironments)
            {
                const float weight = environment.get(trait) * normalization;
                if (weight <= 0.f)
                    continue;
                if (accumulatedWeight <= 0.f)
                {
                    mTarget = mPresets.at(profile.mPalette);
                    mTargetVolumes = profile.mVolumes;
                    accumulatedWeight = weight;
                    continue;
                }
                const float blendFactor = weight / (accumulatedWeight + weight);
                mTarget = blend(mTarget, mPresets.at(profile.mPalette), blendFactor);
                mTargetVolumes = blend(mTargetVolumes, profile.mVolumes, blendFactor);
                accumulatedWeight += weight;
            }
        }

        /// Apply the selected environment without a regional transition for deterministic diagnostics.
        void setDiagnosticEnvironment(const AtmosphereEnvironmentState& environment)
        {
            mDiagnosticEnvironmentActive = true;
            setEnvironment(environment);
            mApplied = mTarget;
            mAppliedVolumes = mTargetVolumes;
            mAppliedTerrainHeightWorldUnits = mTargetTerrainHeightWorldUnits;
            mAppliedTerrainSampleCenterWorldUnits = mTargetTerrainSampleCenterWorldUnits;
            mAppliedTerrainGradient = mTargetTerrainGradient;
            mAppliedValleyDepthWorldUnits = mTargetValleyDepthWorldUnits;
        }

        void update(float elapsedSeconds, bool paused, std::span<Weather> weather)
        {
            if (mDiagnosticEnvironmentActive)
            {
                mApplied = mTarget;
                mAppliedVolumes = mTargetVolumes;
                mAppliedTerrainHeightWorldUnits = mTargetTerrainHeightWorldUnits;
                mAppliedTerrainSampleCenterWorldUnits = mTargetTerrainSampleCenterWorldUnits;
                mAppliedTerrainGradient = mTargetTerrainGradient;
                mAppliedValleyDepthWorldUnits = mTargetValleyDepthWorldUnits;
            }
            else if (!paused)
            {
                const float blendFactor
                    = std::clamp(std::max(0.f, elapsedSeconds) / sRegionTransitionSeconds, 0.f, 1.f);
                mApplied = blend(mApplied, mTarget, blendFactor);
                mAppliedVolumes = blend(mAppliedVolumes, mTargetVolumes, blendFactor);
                mAppliedTerrainHeightWorldUnits
                    = std::lerp(mAppliedTerrainHeightWorldUnits, mTargetTerrainHeightWorldUnits, blendFactor);
                mAppliedTerrainSampleCenterWorldUnits = mTargetTerrainSampleCenterWorldUnits;
                mAppliedTerrainGradient
                    = mAppliedTerrainGradient + (mTargetTerrainGradient - mAppliedTerrainGradient) * blendFactor;
                mAppliedValleyDepthWorldUnits
                    = std::lerp(mAppliedValleyDepthWorldUnits, mTargetValleyDepthWorldUnits, blendFactor);
            }

            if (weather.size() < mApplied.mWeather.size())
                throw std::runtime_error("OpenMWSE atmosphere requires the ten Morrowind weather definitions");
            for (std::size_t i = 0; i < mApplied.mWeather.size(); ++i)
            {
                Weather& target = weather[i];
                const WeatherColors& source = mApplied.mWeather[i];
                setCycle(target.mSkyColor, source.mSky);
                setCycle(target.mFogColor, source.mFog);
                setCycle(target.mAmbientColor, source.mAmbient);
                setCycle(target.mSunColor, source.mSun);
                target.mSunDiscSunsetColor = source.mSunDiscSunset;
            }
        }

        std::unordered_map<std::string, Palette> mPresets;
        std::map<std::string, EnvironmentProfile, std::less<>> mEnvironments;
        std::array<VolumeProfile, sWeatherNames.size()> mVolumes;
        Palette mApplied;
        Palette mTarget;
        VolumeModifiers mAppliedVolumes;
        VolumeModifiers mTargetVolumes;
        float mAppliedTerrainHeightWorldUnits = 0.f;
        float mTargetTerrainHeightWorldUnits = 0.f;
        osg::Vec2f mAppliedTerrainSampleCenterWorldUnits;
        osg::Vec2f mTargetTerrainSampleCenterWorldUnits;
        osg::Vec2f mAppliedTerrainGradient;
        osg::Vec2f mTargetTerrainGradient;
        float mAppliedValleyDepthWorldUnits = 0.f;
        float mTargetValleyDepthWorldUnits = 0.f;
        bool mDiagnosticEnvironmentActive = false;
    };

    AtmospherePalette::AtmospherePalette(const VFS::Manager& vfs)
        : AtmospherePalette(*vfs.get(VFS::Path::NormalizedView("openmwse/atmosphere.json")),
              *vfs.get(VFS::Path::NormalizedView("openmwse/atmosphere-volumes.json")))
    {
    }

    AtmospherePalette::AtmospherePalette(std::istream& paletteStream, std::istream& volumeStream)
        : mImpl(std::make_unique<Impl>(paletteStream, volumeStream))
    {
    }

    AtmospherePalette::~AtmospherePalette() = default;

    void AtmospherePalette::setEnvironment(const AtmosphereEnvironmentState& environment)
    {
        mImpl->setEnvironment(environment);
    }

    void AtmospherePalette::setDiagnosticEnvironment(const AtmosphereEnvironmentState& environment)
    {
        mImpl->setDiagnosticEnvironment(environment);
    }

    void AtmospherePalette::update(float elapsedSeconds, bool paused, std::span<Weather> weather)
    {
        mImpl->update(elapsedSeconds, paused, weather);
    }

    MWRender::AtmosphereState AtmospherePalette::getState(
        std::string_view currentWeather, std::string_view nextWeather, float weatherTransition) const
    {
        const float currentWeight = usesScattering(currentWeather) ? 1.f : 0.f;
        const float nextWeight = nextWeather.empty() ? currentWeight : (usesScattering(nextWeather) ? 1.f : 0.f);
        const float transition = std::clamp(weatherTransition, 0.f, 1.f);
        const float weatherWeight = currentWeight * (1.f - transition) + nextWeight * transition;
        const VolumeProfile& currentVolume = mImpl->mVolumes[weatherIndex(currentWeather)];
        const VolumeProfile& nextVolume
            = nextWeather.empty() ? currentVolume : mImpl->mVolumes[weatherIndex(nextWeather)];
        const WeatherPresentationState presentation
            = makeWeatherPresentationState(currentVolume, nextVolume, transition);
        const VolumeProfile volume = blend(currentVolume, nextVolume, presentation);

        MWRender::AtmosphereState result;
        result.mInScattering = mImpl->mApplied.mInScattering;
        result.mOutScattering = mImpl->mApplied.mOutScattering;
        result.mWeatherWeight = weatherWeight * weatherWeight;
        result.mEnabled = true;
        const VolumeModifiers& environment = mImpl->mAppliedVolumes;
        result.mVolumes = { volume.mCloudDeckCoverage, volume.mCloudDensity * environment.mCloudDensityMultiplier,
            volume.mCloudBaseAltitudeWorldUnits + environment.mCloudAltitudeOffsetWorldUnits,
            volume.mCloudThicknessWorldUnits * environment.mCloudThicknessMultiplier, volume.mCloudType,
            volume.mCloudFeatureScale, volume.mCloudVerticalDevelopment, volume.mCloudAnvilStrength,
            volume.mCloudDeckContinuity, volume.mCloudConvectiveFrequency, volume.mCloudConvectiveMaturity,
            volume.mCloudStormOrganization, volume.mCloudTowerTaper, volume.mCloudBaseVariation,
            volume.mCloudTerrainFollowing, volume.mCloudTerrainClearanceWorldUnits,
            std::clamp(volume.mLowCloudCoverage
                    * std::lerp(1.f, environment.mLowCloudOccurrenceMultiplier, volume.mLowCloudEnvironmentInfluence),
                0.f, 1.f),
            volume.mLowCloudDensity
                * std::lerp(1.f, environment.mLowCloudDensityMultiplier, volume.mLowCloudEnvironmentInfluence),
            volume.mLowCloudBaseAltitudeWorldUnits
                + environment.mLowCloudAltitudeOffsetWorldUnits * volume.mLowCloudEnvironmentInfluence,
            volume.mLowCloudThicknessWorldUnits
                * std::lerp(1.f, environment.mLowCloudThicknessMultiplier, volume.mLowCloudEnvironmentInfluence),
            volume.mLowCloudType, volume.mLowCloudFeatureScale, volume.mLowCloudVerticalDevelopment,
            volume.mLowCloudEnvironmentInfluence, mImpl->mAppliedTerrainHeightWorldUnits,
            mImpl->mAppliedTerrainSampleCenterWorldUnits, mImpl->mAppliedTerrainGradient,
            mImpl->mAppliedValleyDepthWorldUnits, volume.mMistDensity * environment.mMistDensityMultiplier,
            volume.mMistBaseAltitudeWorldUnits + environment.mMistAltitudeOffsetWorldUnits,
            volume.mMistThicknessWorldUnits * environment.mMistThicknessMultiplier, volume.mShaftIntensity,
            volume.mFogDensity * environment.mMistDensityMultiplier,
            volume.mFogBaseAltitudeWorldUnits + environment.mMistAltitudeOffsetWorldUnits,
            volume.mFogThicknessWorldUnits * environment.mMistThicknessMultiplier, volume.mFogAnisotropy };
        return result;
    }
}
