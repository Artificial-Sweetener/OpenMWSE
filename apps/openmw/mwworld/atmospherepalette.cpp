#include "atmospherepalette.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
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

        /// Keep authored weather volumes in the same typed representation published to rendering.
        using VolumeProfile = MWRender::AtmosphereVolumeState;

        /// Preserve source formation and optical depth under a uniform physical scale.
        void scaleCloudModel(SceneUtil::CloudLayer& layer, float scale)
        {
            layer.mPhysicalScale *= scale;
            layer.mExtinctionPerMeter /= scale;
            layer.mNoiseScalePerMeter /= scale;
            layer.mWeatherScalePerMeter /= scale;
            layer.mShapeSkewMeters *= scale;
            layer.mCoverageSkewMeters *= scale;
            layer.mCurlDisplacementMeters *= scale;
            layer.mWindSpeedMetersPerSecond *= scale;
            layer.mCoverageWindSpeedMetersPerSecond *= scale;
        }

        /// Read a complete source layer with optional physical scaling and independently authored cloud controls.
        SceneUtil::CloudLayer readCloudModel(const YAML::Node& source, std::string_view path)
        {
            if (!source.IsMap())
                throw std::runtime_error("OpenMWSE cloud layer must be a map: " + std::string(path));
            const std::string profile = source["profile"] ? source["profile"].as<std::string>() : "primary";
            if (profile != "primary" && profile != "secondary" && profile != "fair-weather")
                throw std::runtime_error("Unknown cloud layer profile: " + std::string(path));
            SceneUtil::CloudLayer layer = profile == "secondary" ? SceneUtil::secondaryCloudLayer()
                : profile == "fair-weather"                      ? SceneUtil::fairWeatherCloudLayer()
                                                                 : SceneUtil::CloudLayer{};
            if (source["physicalScale"])
                scaleCloudModel(layer, readBoundedFloat(source, "physicalScale", path, .1f, 4.f));
            const auto scalar = [&](std::string_view name, float& target, float minimum, float maximum) {
                if (source[std::string(name)])
                    target = readBoundedFloat(source, name, path, minimum, maximum);
            };
            scalar("skewAlongWindDirection", layer.mShapeSkewMeters, 0.f, 10000.f);
            scalar("totalNoiseScale", layer.mNoiseScalePerMeter, .00001f, .02f);
            scalar("curlScale", layer.mCurlScale, .001f, 10.f);
            scalar("curlNoiseHeightFraction", layer.mCurlHeightExponent, 0.f, 1000.f);
            scalar("curlNoiseModifier", layer.mCurlDisplacementMeters, 0.f, 10000.f);
            scalar("detailScale", layer.mDetailScale, .1f, 32.f);
            scalar("detailNoiseHeightFraction", layer.mDetailHeightFraction, 0.f, 1000.f);
            scalar("detailNoiseModifier", layer.mDetailErosion, 0.f, 1.f);
            scalar("skewAlongCoverageWindDirection", layer.mCoverageSkewMeters, 0.f, 20000.f);
            scalar("weatherScale", layer.mWeatherScalePerMeter, .000001f, .01f);
            scalar("coverageAmount", layer.mCoverageAmount, 0.f, 10.f);
            scalar("coverageMinimum", layer.mCoverageMinimum, 0.f, 1.f);
            scalar("typeAmount", layer.mTypeAmount, 0.f, 10.f);
            scalar("typeMinimum", layer.mTypeMinimum, 0.f, 1.f);
            scalar("rainAmount", layer.mRainAmount, 0.f, 4.f);
            scalar("rainMinimum", layer.mRainMinimum, 0.f, 1.f);
            scalar("windSpeed", layer.mWindSpeedMetersPerSecond, -100.f, 100.f);
            scalar("windAngle", layer.mWindAngleRadians, -6.283186f, 6.283186f);
            scalar("windUpAmount", layer.mWindUpAmount, -4.f, 4.f);
            scalar("coverageWindSpeed", layer.mCoverageWindSpeedMetersPerSecond, -100.f, 100.f);
            scalar("coverageWindAngle", layer.mCoverageWindAngleRadians, -6.283186f, 6.283186f);
            const auto vector = [&](std::string_view name, osg::Vec4f& target, bool gradient) {
                const YAML::Node value = source[std::string(name)];
                if (!value)
                    return;
                if (!value.IsSequence() || value.size() != 4)
                    throw std::runtime_error(
                        "Cloud profile requires four values: " + std::string(path) + "." + std::string(name));
                for (unsigned int index = 0; index < 4; ++index)
                {
                    target[index] = value[index].as<float>();
                    if (!std::isfinite(target[index]) || target[index] < 0.f
                        || target[index] > (gradient ? 1.f : 1000.f))
                        throw std::runtime_error(
                            "Cloud profile value outside range: " + std::string(path) + "." + std::string(name));
                }
                if (gradient && !(target.x() < target.y() && target.y() <= target.z() && target.z() < target.w()))
                    throw std::runtime_error("Cloud gradient requires start < fade-in <= fade-out < end: "
                        + std::string(path) + "." + std::string(name));
            };
            vector("gradientSmall", layer.mGradientSmall, true);
            vector("gradientMedium", layer.mGradientMedium, true);
            vector("gradientLarge", layer.mGradientLarge, true);
            vector("anvilDeformationSmall", layer.mAnvilSmall, false);
            vector("anvilDeformationMedium", layer.mAnvilMedium, false);
            vector("anvilDeformationLarge", layer.mAnvilLarge, false);
            if (source["albedo"])
                layer.mAlbedo = readVec3(source["albedo"], std::string(path) + ".albedo");
            if (source["extinctionCoefficient"])
                layer.mExtinctionPerMeter
                    = readVec3(source["extinctionCoefficient"], std::string(path) + ".extinctionCoefficient");
            for (unsigned int channel = 0; channel < 3; ++channel)
                if (layer.mAlbedo[channel] < 0.f || layer.mAlbedo[channel] > 1.f
                    || layer.mExtinctionPerMeter[channel] < 0.f || layer.mExtinctionPerMeter[channel] > 1.f)
                    throw std::runtime_error("Cloud optical coefficient outside range: " + std::string(path));
            return layer;
        }

        /// Parse bounded world-unit weather profiles, including deep convective shells, with field-qualified errors.
        VolumeProfile readVolumeProfile(const YAML::Node& source, std::string_view path)
        {
            if (!source.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere data is missing volume profile: " + std::string(path));
            VolumeProfile result;
            result.mCloudDeckCoverage = readBoundedFloat(source, "cloudDeckCoverage", path, 0.0f, 1.0f);
            result.mCloudDensity = readBoundedFloat(source, "cloudDensity", path, 0.0f, 4.0f);
            result.mCloudBaseAltitudeWorldUnits = readBoundedFloat(source, "cloudAltitude", path, -10000.0f, 200000.0f);
            result.mCloudThicknessWorldUnits = readBoundedFloat(source, "cloudThickness", path, 1.0f, 350000.0f);
            result.mCloudTerrainFollowing = readBoundedFloat(source, "cloudTerrainFollowing", path, 0.0f, 1.0f);
            result.mCloudTerrainClearanceWorldUnits
                = readBoundedFloat(source, "cloudTerrainClearance", path, -20000.0f, 100000.0f);
            result.mLowCloudCoverage = readBoundedFloat(source, "lowCloudCoverage", path, 0.0f, 1.0f);
            result.mLowCloudDensity = readBoundedFloat(source, "lowCloudDensity", path, 0.0f, 4.0f);
            result.mLowCloudBaseAltitudeWorldUnits
                = readBoundedFloat(source, "lowCloudAltitude", path, -10000.0f, 50000.0f);
            result.mLowCloudThicknessWorldUnits = readBoundedFloat(source, "lowCloudThickness", path, 1.0f, 100000.0f);
            result.mLowCloudEnvironmentInfluence
                = readBoundedFloat(source, "lowCloudEnvironmentInfluence", path, 0.0f, 1.0f);
            result.mMistDensity = readBoundedFloat(source, "mistDensity", path, 0.0f, 4.0f);
            result.mMistBaseAltitudeWorldUnits = readBoundedFloat(source, "mistAltitude", path, -10000.0f, 50000.0f);
            result.mMistThicknessWorldUnits = readBoundedFloat(source, "mistThickness", path, 1.0f, 100000.0f);
            result.mShaftIntensity = readBoundedFloat(source, "shaftIntensity", path, 0.0f, 4.0f);
            result.mFogDensity = readBoundedFloat(source, "fogDensity", path, 0.0f, 4.0f);
            result.mFogBaseAltitudeWorldUnits = readBoundedFloat(source, "fogAltitude", path, -10000.0f, 50000.0f);
            result.mFogThicknessWorldUnits = readBoundedFloat(source, "fogThickness", path, 1.0f, 100000.0f);
            result.mFogAnisotropy = readBoundedFloat(source, "fogAnisotropy", path, 0.0f, 0.95f);
            if (source["cloudLayer"])
                result.mCloudLayer = readCloudModel(source["cloudLayer"], std::string(path) + ".cloudLayer");
            else
            {
                // Existing mod-authored volume files retain their placement and approximate coverage intent.
                result.mCloudLayer.mCoverageAmount = 2.f * result.mCloudDeckCoverage;
                result.mCloudLayer.mTypeMinimum = readBoundedFloat(source, "cloudDeckType", path, 0.f, 1.f);
                scaleCloudModel(result.mCloudLayer, readBoundedFloat(source, "cloudFeatureScale", path, .25f, 4.f));
            }
            if (source["environmentalCloudLayer"])
                result.mEnvironmentalCloudLayer
                    = readCloudModel(source["environmentalCloudLayer"], std::string(path) + ".environmentalCloudLayer");
            else
            {
                scaleCloudModel(result.mEnvironmentalCloudLayer, .18f);
                result.mEnvironmentalCloudLayer.mCoverageAmount = result.mLowCloudCoverage * 2.f;
                result.mEnvironmentalCloudLayer.mTypeMinimum = .15f;
            }
            return result;
        }

        /// Return a Hermite transition over the complete authoritative weather interval.
        float smoothTransition(float value)
        {
            const float normalized = std::clamp(value, 0.f, 1.f);
            return normalized * normalized * (3.f - 2.f * normalized);
        }

        /// Describe one authored sky family using a complete cloud layer and a physical shell.
        struct CloudScenarioProfile
        {
            SceneUtil::CloudLayer mLayer;
            float mBaseAltitudeMeters = 0.f;
            float mThicknessMeters = 1.f;
            bool mCloudless = false;
            bool mInheritRain = true;
        };

        /// Validate an authored family before any episode can select it.
        CloudScenarioProfile readCloudScenario(const YAML::Node& source, const std::string& path)
        {
            if (!source.IsMap())
                throw std::runtime_error("Cloud scenario must be a map: " + path);
            CloudScenarioProfile result;
            result.mCloudless = source["cloudless"] && source["cloudless"].as<bool>();
            if (result.mCloudless)
                return result;
            result.mBaseAltitudeMeters = readBoundedFloat(source, "baseAltitudeMeters", path, 0.f, 2800.f);
            result.mThicknessMeters = readBoundedFloat(source, "thicknessMeters", path, 1.f, 4500.f);
            result.mLayer = readCloudModel(source["cloudLayer"], path + ".cloudLayer");
            result.mInheritRain = !source["cloudLayer"]["rainAmount"] && !source["cloudLayer"]["rainMinimum"];
            return result;
        }

        /// Return a deterministic unit variate for bounded differences between episodes of one family.
        float cloudScenarioVariation(std::uint32_t identity, std::uint32_t salt)
        {
            std::uint32_t bits = identity ^ (salt * 0x9e3779b9u);
            bits ^= bits >> 16u;
            bits *= 0x7feb352du;
            bits ^= bits >> 15u;
            bits *= 0x846ca68bu;
            bits ^= bits >> 16u;
            return static_cast<float>(bits & 0xffffu) / 65535.f;
        }

        /// Realize stable weather episodes from authored source models without changing either transition endpoint.
        VolumeProfile realizeCloudScenario(VolumeProfile profile,
            const std::array<std::optional<CloudScenarioProfile>, 8>& scenarios, std::uint32_t identity)
        {
            if (identity == 0)
                return profile;
            constexpr float unitsPerMeter = 69.99125109f;
            const auto& scenario = scenarios[identity % scenarios.size()];
            if (scenario)
            {
                if (scenario->mCloudless)
                {
                    profile.mCloudDeckCoverage = 0.f;
                    profile.mCloudDensity = 0.f;
                    profile.mLowCloudCoverage = 0.f;
                    profile.mCloudLayer.mCoverageAmount = 0.f;
                    profile.mCloudLayer.mCoverageMinimum = 0.f;
                    profile.mEnvironmentalCloudLayer.mCoverageAmount = 0.f;
                    profile.mEnvironmentalCloudLayer.mCoverageMinimum = 0.f;
                    return profile;
                }
                SceneUtil::CloudLayer model = scenario->mLayer;
                if (scenario->mInheritRain)
                {
                    model.mRainAmount = profile.mCloudLayer.mRainAmount;
                    model.mRainMinimum = profile.mCloudLayer.mRainMinimum;
                }
                profile.mCloudLayer = model;
                profile.mCloudBaseAltitudeWorldUnits = scenario->mBaseAltitudeMeters * unitsPerMeter;
                profile.mCloudThicknessWorldUnits = scenario->mThicknessMeters * unitsPerMeter;
            }
            profile.mCloudLayer.mCoverageAmount *= std::lerp(.85f, 1.15f, cloudScenarioVariation(identity, 1u));
            profile.mCloudBaseAltitudeWorldUnits
                += std::lerp(-100.f, 100.f, cloudScenarioVariation(identity, 2u)) * unitsPerMeter;
            const float scaleVariation = std::lerp(.9f, 1.1f, cloudScenarioVariation(identity, 3u));
            scaleCloudModel(profile.mCloudLayer, scaleVariation);
            profile.mCloudThicknessWorldUnits *= scaleVariation;
            profile.mCloudLayer.mCoverageWindAngleRadians += std::lerp(-.3f, .3f, cloudScenarioVariation(identity, 4u));
            return profile;
        }

        /// Evolve environmental coverage and shaping continuously within the shared world-space field.
        SceneUtil::CloudLayer blendEnvironmentalCloudLayer(
            const SceneUtil::CloudLayer& from, const SceneUtil::CloudLayer& to, float factor)
        {
            SceneUtil::CloudLayer result;
            result.mPhysicalScale = std::lerp(from.mPhysicalScale, to.mPhysicalScale, factor);
            for (int channel = 0; channel < 3; ++channel)
                result.mAlbedo[channel] = std::lerp(from.mAlbedo[channel], to.mAlbedo[channel], factor);
            for (int channel = 0; channel < 3; ++channel)
                result.mExtinctionPerMeter[channel]
                    = std::lerp(from.mExtinctionPerMeter[channel], to.mExtinctionPerMeter[channel], factor);
            result.mShapeSkewMeters = std::lerp(from.mShapeSkewMeters, to.mShapeSkewMeters, factor);
            result.mNoiseScalePerMeter = std::lerp(from.mNoiseScalePerMeter, to.mNoiseScalePerMeter, factor);
            result.mCurlScale = std::lerp(from.mCurlScale, to.mCurlScale, factor);
            result.mCurlHeightExponent = std::lerp(from.mCurlHeightExponent, to.mCurlHeightExponent, factor);
            result.mCurlDisplacementMeters
                = std::lerp(from.mCurlDisplacementMeters, to.mCurlDisplacementMeters, factor);
            result.mDetailScale = std::lerp(from.mDetailScale, to.mDetailScale, factor);
            result.mDetailHeightFraction = std::lerp(from.mDetailHeightFraction, to.mDetailHeightFraction, factor);
            result.mDetailErosion = std::lerp(from.mDetailErosion, to.mDetailErosion, factor);
            result.mCoverageSkewMeters = std::lerp(from.mCoverageSkewMeters, to.mCoverageSkewMeters, factor);
            result.mWeatherScalePerMeter = std::lerp(from.mWeatherScalePerMeter, to.mWeatherScalePerMeter, factor);
            result.mCoverageAmount = std::lerp(from.mCoverageAmount, to.mCoverageAmount, factor);
            result.mCoverageMinimum = std::lerp(from.mCoverageMinimum, to.mCoverageMinimum, factor);
            result.mTypeAmount = std::lerp(from.mTypeAmount, to.mTypeAmount, factor);
            result.mTypeMinimum = std::lerp(from.mTypeMinimum, to.mTypeMinimum, factor);
            result.mRainAmount = std::lerp(from.mRainAmount, to.mRainAmount, factor);
            result.mRainMinimum = std::lerp(from.mRainMinimum, to.mRainMinimum, factor);
            for (int channel = 0; channel < 4; ++channel)
                result.mGradientSmall[channel]
                    = std::lerp(from.mGradientSmall[channel], to.mGradientSmall[channel], factor);
            for (int channel = 0; channel < 4; ++channel)
                result.mGradientMedium[channel]
                    = std::lerp(from.mGradientMedium[channel], to.mGradientMedium[channel], factor);
            for (int channel = 0; channel < 4; ++channel)
                result.mGradientLarge[channel]
                    = std::lerp(from.mGradientLarge[channel], to.mGradientLarge[channel], factor);
            for (int channel = 0; channel < 4; ++channel)
                result.mAnvilSmall[channel] = std::lerp(from.mAnvilSmall[channel], to.mAnvilSmall[channel], factor);
            for (int channel = 0; channel < 4; ++channel)
                result.mAnvilMedium[channel] = std::lerp(from.mAnvilMedium[channel], to.mAnvilMedium[channel], factor);
            for (int channel = 0; channel < 4; ++channel)
                result.mAnvilLarge[channel] = std::lerp(from.mAnvilLarge[channel], to.mAnvilLarge[channel], factor);
            result.mWindSpeedMetersPerSecond
                = std::lerp(from.mWindSpeedMetersPerSecond, to.mWindSpeedMetersPerSecond, factor);
            result.mWindAngleRadians = std::lerp(from.mWindAngleRadians, to.mWindAngleRadians, factor);
            result.mWindUpAmount = std::lerp(from.mWindUpAmount, to.mWindUpAmount, factor);
            result.mCoverageWindSpeedMetersPerSecond
                = std::lerp(from.mCoverageWindSpeedMetersPerSecond, to.mCoverageWindSpeedMetersPerSecond, factor);
            result.mCoverageWindAngleRadians
                = std::lerp(from.mCoverageWindAngleRadians, to.mCoverageWindAngleRadians, factor);
            return result;
        }

        /// Interpolate resolved episode volumes over the full authoritative weather transition.
        VolumeProfile blend(const VolumeProfile& from, const VolumeProfile& to, float factor)
        {
            VolumeProfile result = factor < .5f ? from : to;
            result.mEnvironmentalCloudLayer
                = blendEnvironmentalCloudLayer(from.mEnvironmentalCloudLayer, to.mEnvironmentalCloudLayer, factor);
            result.mCloudDeckCoverage = std::lerp(from.mCloudDeckCoverage, to.mCloudDeckCoverage, factor);
            result.mCloudDensity = std::lerp(from.mCloudDensity, to.mCloudDensity, factor);
            result.mCloudBaseAltitudeWorldUnits
                = std::lerp(from.mCloudBaseAltitudeWorldUnits, to.mCloudBaseAltitudeWorldUnits, factor);
            result.mCloudThicknessWorldUnits
                = std::lerp(from.mCloudThicknessWorldUnits, to.mCloudThicknessWorldUnits, factor);
            result.mCloudTerrainFollowing = std::lerp(from.mCloudTerrainFollowing, to.mCloudTerrainFollowing, factor);
            result.mCloudTerrainClearanceWorldUnits
                = std::lerp(from.mCloudTerrainClearanceWorldUnits, to.mCloudTerrainClearanceWorldUnits, factor);
            result.mLowCloudCoverage = std::lerp(from.mLowCloudCoverage, to.mLowCloudCoverage, factor);
            result.mLowCloudDensity = std::lerp(from.mLowCloudDensity, to.mLowCloudDensity, factor);
            result.mLowCloudBaseAltitudeWorldUnits
                = std::lerp(from.mLowCloudBaseAltitudeWorldUnits, to.mLowCloudBaseAltitudeWorldUnits, factor);
            result.mLowCloudThicknessWorldUnits
                = std::lerp(from.mLowCloudThicknessWorldUnits, to.mLowCloudThicknessWorldUnits, factor);
            result.mLowCloudEnvironmentInfluence
                = std::lerp(from.mLowCloudEnvironmentInfluence, to.mLowCloudEnvironmentInfluence, factor);
            result.mMistDensity = std::lerp(from.mMistDensity, to.mMistDensity, factor);
            result.mMistBaseAltitudeWorldUnits
                = std::lerp(from.mMistBaseAltitudeWorldUnits, to.mMistBaseAltitudeWorldUnits, factor);
            result.mMistThicknessWorldUnits
                = std::lerp(from.mMistThicknessWorldUnits, to.mMistThicknessWorldUnits, factor);
            result.mShaftIntensity = std::lerp(from.mShaftIntensity, to.mShaftIntensity, factor);
            result.mFogDensity = std::lerp(from.mFogDensity, to.mFogDensity, factor);
            result.mFogBaseAltitudeWorldUnits
                = std::lerp(from.mFogBaseAltitudeWorldUnits, to.mFogBaseAltitudeWorldUnits, factor);
            result.mFogThicknessWorldUnits
                = std::lerp(from.mFogThicknessWorldUnits, to.mFogThicknessWorldUnits, factor);
            result.mFogAnisotropy = std::lerp(from.mFogAnisotropy, to.mFogAnisotropy, factor);
            return result;
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

            std::unordered_map<std::string, CloudScenarioProfile> scenarios;
            const YAML::Node authoredScenarios = volumes["cloudScenarios"];
            if (authoredScenarios)
            {
                if (!authoredScenarios.IsMap() || authoredScenarios.size() > 128)
                    throw std::runtime_error("atmosphere-volumes.cloudScenarios requires at most 128 named profiles");
                for (const auto& entry : authoredScenarios)
                {
                    const std::string name = entry.first.as<std::string>();
                    scenarios.emplace(
                        name, readCloudScenario(entry.second, "atmosphere-volumes.cloudScenarios." + name));
                }
            }

            for (std::size_t i = 0; i < sWeatherNames.size(); ++i)
            {
                const std::string weatherName(sWeatherNames[i]);
                mVolumes[i] = readVolumeProfile(volumes[weatherName], "atmosphere-volumes." + weatherName);
                mCloudScenarioWeights[i].fill(1u);
                const YAML::Node choices = volumes[weatherName]["cloudScenarios"];
                if (choices)
                {
                    const std::string path = "atmosphere-volumes." + weatherName + ".cloudScenarios";
                    if (!choices.IsSequence() || choices.size() != mCloudScenarios[i].size())
                        throw std::runtime_error("Cloud weather requires eight episode slots: " + path);
                    for (std::size_t slot = 0; slot < choices.size(); ++slot)
                    {
                        const YAML::Node choice = choices[slot];
                        const std::string name
                            = choice.IsMap() ? choice["family"].as<std::string>() : choice.as<std::string>();
                        if (choice.IsMap())
                        {
                            const auto weight = choice["weight"].as<std::uint32_t>();
                            if (weight == 0u || weight > 1000u)
                                throw std::runtime_error("Cloud episode weight must be from 1 to 1000: " + path);
                            mCloudScenarioWeights[i][slot] = weight;
                        }
                        const auto found = scenarios.find(name);
                        if (found == scenarios.end())
                            throw std::runtime_error("Unknown cloud scenario " + name + ": " + path);
                        mCloudScenarios[i][slot] = found->second;
                    }
                }
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

            const YAML::Node regions = root["regions"];
            if (regions && !regions.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere regions must be a map");
            for (const auto& entry : regions)
            {
                const std::string preset = entry.second.as<std::string>();
                if (!mPresets.contains(preset))
                    throw std::runtime_error("OpenMWSE atmosphere region references an unknown preset: " + preset);
                mRegions.emplace(lowerCase(entry.first.as<std::string>()), preset);
            }
            mRegionalFrom = mApplied = mTarget = defaultPreset->second;
        }

        /// Resolve the active color trajectory without affecting weather volume state.
        Palette colors() const
        {
            return mColorSource == AtmosphereColorSource::Region
                ? blend(mRegionalFrom, mPresets.at(mRegionalTarget), mRegionalTransition)
                : mApplied;
        }

        /// Preserve the original fixed-duration interpolation between regional palettes.
        void setRegion(std::string_view regionId)
        {
            const auto region = mRegions.find(lowerCase(regionId));
            const std::string next = region == mRegions.end() ? "default" : region->second;
            if (next == mRegionalTarget)
                return;
            mRegionalFrom = blend(mRegionalFrom, mPresets.at(mRegionalTarget), mRegionalTransition);
            mRegionalTarget = next;
            mRegionalTransition = 0.f;
        }

        /// Resolve environmental color and volume targets from their shared local evidence.
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

        /// Blend regional evidence while expressing both terrain planes relative to the same sampling origin.
        void update(float elapsedSeconds, bool paused, std::span<Weather> weather)
        {
            if (!paused)
                mRegionalTransition
                    = std::min(1.f, mRegionalTransition + std::max(0.f, elapsedSeconds) / sRegionTransitionSeconds);
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
                // Moving the sample origin changes the plane's intercept even when the terrain is unchanged.
                mAppliedTerrainHeightWorldUnits
                    += (mTargetTerrainSampleCenterWorldUnits - mAppliedTerrainSampleCenterWorldUnits)
                    * mAppliedTerrainGradient;
                mAppliedTerrainHeightWorldUnits
                    = std::lerp(mAppliedTerrainHeightWorldUnits, mTargetTerrainHeightWorldUnits, blendFactor);
                mAppliedTerrainSampleCenterWorldUnits = mTargetTerrainSampleCenterWorldUnits;
                mAppliedTerrainGradient
                    = mAppliedTerrainGradient + (mTargetTerrainGradient - mAppliedTerrainGradient) * blendFactor;
                mAppliedValleyDepthWorldUnits
                    = std::lerp(mAppliedValleyDepthWorldUnits, mTargetValleyDepthWorldUnits, blendFactor);
            }

            const Palette selected = colors();
            if (weather.size() < selected.mWeather.size())
                throw std::runtime_error("OpenMWSE atmosphere requires the ten Morrowind weather definitions");
            for (std::size_t i = 0; i < selected.mWeather.size(); ++i)
            {
                Weather& target = weather[i];
                const WeatherColors& source = selected.mWeather[i];
                setCycle(target.mSkyColor, source.mSky);
                setCycle(target.mFogColor, source.mFog);
                setCycle(target.mAmbientColor, source.mAmbient);
                setCycle(target.mSunColor, source.mSun);
                target.mSunDiscSunsetColor = source.mSunDiscSunset;
            }
        }

        /// Visit the complete transient regional trajectory with stable, named diagnostic fields.
        template <class Self, class Visitor>
        static void visitRuntime(Self& self, unsigned version, Visitor&& visit)
        {
            const auto palette = [&](const std::string& prefix, auto& value) {
                visit(prefix + "-in-scattering", value.mInScattering);
                visit(prefix + "-out-scattering", value.mOutScattering);
                for (std::size_t i = 0; i < value.mWeather.size(); ++i)
                {
                    auto& weather = value.mWeather[i];
                    const std::string base = prefix + "-" + std::string(sWeatherNames[i]);
                    const auto cycle = [&](const std::string& name, auto& colors) {
                        visit(name + "-sunrise", colors.mSunrise);
                        visit(name + "-day", colors.mDay);
                        visit(name + "-sunset", colors.mSunset);
                        visit(name + "-night", colors.mNight);
                    };
                    cycle(base + "-sky", weather.mSky);
                    cycle(base + "-fog", weather.mFog);
                    cycle(base + "-ambient", weather.mAmbient);
                    cycle(base + "-sun", weather.mSun);
                    visit(base + "-sun-disc", weather.mSunDiscSunset);
                }
            };
            const auto volumes = [&](const std::string& prefix, auto& value) {
                visit(prefix + "-cloud-density", value.mCloudDensityMultiplier);
                visit(prefix + "-cloud-altitude", value.mCloudAltitudeOffsetWorldUnits);
                visit(prefix + "-cloud-thickness", value.mCloudThicknessMultiplier);
                visit(prefix + "-low-occurrence", value.mLowCloudOccurrenceMultiplier);
                visit(prefix + "-low-density", value.mLowCloudDensityMultiplier);
                visit(prefix + "-low-altitude", value.mLowCloudAltitudeOffsetWorldUnits);
                visit(prefix + "-low-thickness", value.mLowCloudThicknessMultiplier);
                visit(prefix + "-mist-density", value.mMistDensityMultiplier);
                visit(prefix + "-mist-altitude", value.mMistAltitudeOffsetWorldUnits);
                visit(prefix + "-mist-thickness", value.mMistThicknessMultiplier);
            };
            if (version >= 2)
            {
                palette("regional-from", self.mRegionalFrom);
                visit("regional-transition", self.mRegionalTransition);
            }
            palette("applied", self.mApplied);
            palette("target", self.mTarget);
            volumes("applied-volumes", self.mAppliedVolumes);
            volumes("target-volumes", self.mTargetVolumes);
            visit("applied-terrain-height", self.mAppliedTerrainHeightWorldUnits);
            visit("applied-terrain-center", self.mAppliedTerrainSampleCenterWorldUnits);
            visit("applied-terrain-gradient", self.mAppliedTerrainGradient);
            visit("applied-valley-depth", self.mAppliedValleyDepthWorldUnits);
            visit("target-terrain-height", self.mTargetTerrainHeightWorldUnits);
            visit("target-terrain-center", self.mTargetTerrainSampleCenterWorldUnits);
            visit("target-terrain-gradient", self.mTargetTerrainGradient);
            visit("target-valley-depth", self.mTargetValleyDepthWorldUnits);
        }

        std::unordered_map<std::string, Palette> mPresets;
        std::map<std::string, EnvironmentProfile, std::less<>> mEnvironments;
        std::array<VolumeProfile, sWeatherNames.size()> mVolumes;
        std::array<std::array<std::optional<CloudScenarioProfile>, 8>, sWeatherNames.size()> mCloudScenarios;
        std::array<std::array<std::uint32_t, 8>, sWeatherNames.size()> mCloudScenarioWeights;
        AtmosphereColorSource mColorSource = AtmosphereColorSource::Environment;
        std::unordered_map<std::string, std::string> mRegions;
        Palette mRegionalFrom;
        std::string mRegionalTarget = "default";
        float mRegionalTransition = 1.f;
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

    void AtmospherePalette::setColorSource(AtmosphereColorSource source)
    {
        mImpl->mColorSource = source;
    }

    void AtmospherePalette::setRegion(std::string_view regionId)
    {
        mImpl->setRegion(regionId);
    }

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

    std::string AtmospherePalette::captureRuntimeState() const
    {
        YAML::Node state;
        state["runtime-version"] = 2;
        state["regional-colors"]["target"] = mImpl->mRegionalTarget;
        Impl::visitRuntime(std::as_const(*mImpl), 2, [&](const std::string& name, const auto& value) {
            YAML::Node destination
                = name.starts_with("regional-") ? state["regional-colors"][name.substr(9)] : state[name];
            if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(value)>>)
                destination = value;
            else
                for (unsigned i = 0; i < value.num_components; ++i)
                    destination.push_back(value[i]);
        });
        state["diagnostic-environment"] = mImpl->mDiagnosticEnvironmentActive;
        YAML::Emitter output;
        output.SetFloatPrecision(std::numeric_limits<float>::max_digits10);
        output << state;
        return output.c_str();
    }

    void AtmospherePalette::restoreRuntimeState(std::string_view text)
    {
        const YAML::Node state = YAML::Load(std::string(text));
        if (!state.IsMap())
            throw std::invalid_argument("unsupported atmosphere palette runtime state");
        const unsigned version = state["runtime-version"].as<unsigned>();
        if (version != 1 && version != 2)
            throw std::invalid_argument("unsupported atmosphere palette runtime state");
        const std::string regionalTarget
            = version == 2 ? state["regional-colors"]["target"].as<std::string>() : "default";
        if (!mImpl->mPresets.contains(regionalTarget))
            throw std::invalid_argument("unknown regional atmosphere runtime palette: " + regionalTarget);
        const bool diagnosticEnvironment = state["diagnostic-environment"].as<bool>();
        // Validate the entire snapshot before replacing any part of the regional trajectory.
        for (const bool apply : { false, true })
        {
            std::size_t fieldCount = version == 2 ? 3 : 2;
            std::size_t regionalFieldCount = 1;
            Impl::visitRuntime(*mImpl, version, [&](const std::string& name, auto& value) {
                const bool regional = name.starts_with("regional-");
                ++(regional ? regionalFieldCount : fieldCount);
                const YAML::Node source = regional ? state["regional-colors"][name.substr(9)] : state[name];
                const auto read = [&](const YAML::Node& node) {
                    const float result = node.as<float>();
                    if (!std::isfinite(result))
                        throw std::invalid_argument("non-finite atmosphere runtime field: " + name);
                    return result;
                };
                if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(value)>>)
                {
                    const float restored = read(source);
                    if (name.find("-volumes-") != std::string::npos && name.find("altitude") == std::string::npos
                        && (restored < 0.f || restored > 4.f))
                        throw std::invalid_argument("invalid atmosphere volume multiplier: " + name);
                    if (name == "regional-transition" && (restored < 0.f || restored > 1.f))
                        throw std::invalid_argument("invalid regional atmosphere transition");
                    if (apply)
                        value = restored;
                }
                else
                {
                    if (!source.IsSequence() || source.size() != value.num_components)
                        throw std::invalid_argument("invalid atmosphere runtime vector: " + name);
                    for (unsigned i = 0; i < value.num_components; ++i)
                    {
                        const float restored = read(source[i]);
                        if (apply)
                            value[i] = restored;
                    }
                }
            });
            if (state.size() != fieldCount || (version == 2 && state["regional-colors"].size() != regionalFieldCount))
                throw std::invalid_argument("unexpected atmosphere runtime fields");
        }
        mImpl->mRegionalTarget = regionalTarget;
        if (version == 1)
        {
            // Older captures predate the regional color trajectory; preserve all recorded environment state.
            mImpl->mRegionalFrom = mImpl->mPresets.at("default");
            mImpl->mRegionalTransition = 1.f;
        }
        mImpl->mDiagnosticEnvironmentActive = diagnosticEnvironment;
    }

    std::uint32_t AtmospherePalette::createCloudScenarioIdentity(std::string_view weather,
        std::string_view originWeather, std::uint32_t sequence, float recentPrecipitation, float gameHour,
        float sunriseHour, const AtmosphereEnvironmentState& environment) const
    {
        const std::size_t target = weatherIndex(weather);
        const std::size_t origin = originWeather.empty() ? sWeatherNames.size() : weatherIndex(originWeather);
        std::uint32_t identity = sequence;
        identity ^= static_cast<std::uint32_t>(target + 1u) * 0x9e3779b9u;
        identity ^= static_cast<std::uint32_t>(origin + 1u) * 0x85ebca6bu;
        identity ^= identity >> 16u;
        identity *= 0x7feb352du;
        identity ^= identity >> 15u;
        identity *= 0x846ca68bu;
        identity ^= identity >> 16u;

        const float retainedMoisture = std::clamp(recentPrecipitation, 0.f, 1.f);
        const float humidEnvironment = std::clamp(
            environment.get("wetland") * 0.55f + environment.get("coastal") * 0.25f + retainedMoisture * 0.6f, 0.f,
            1.f);
        const float normalizedHourDistance = std::fmod(std::abs(gameHour - sunriseHour), 24.f);
        const float wrappedHourDistance = std::min(normalizedHourDistance, 24.f - normalizedHourDistance);
        const float dawnCooling = 1.f - std::clamp((wrappedHourDistance - 1.f) / 3.f, 0.f, 1.f);
        const bool wetOrigin = origin < sWeatherNames.size()
            && (origin == weatherIndex("Rain") || origin == weatherIndex("Thunderstorm")
                || origin == weatherIndex("Snow") || origin == weatherIndex("Blizzard"));
        const auto contextualSlot
            = [&](std::array<std::uint32_t, 4> choices) { return choices.at((identity >> 3u) % choices.size()); };

        std::uint32_t slot = identity & 7u;
        const auto& weights = mImpl->mCloudScenarioWeights[target];
        if (!std::ranges::all_of(weights, [](auto weight) { return weight == 1u; }))
        {
            std::uint32_t totalWeight = 0;
            for (const auto weight : weights)
                totalWeight += weight;
            auto selection = (identity >> 3u) % totalWeight;
            slot = 0;
            while (selection >= weights[slot])
                selection -= weights[slot++];
        }
        if (target == weatherIndex("Clear") && (wetOrigin || retainedMoisture > 0.45f))
            slot = contextualSlot({ 1u, 3u, 5u, 6u });
        else if (target == weatherIndex("Cloudy"))
        {
            const float influence = std::max(humidEnvironment, std::clamp(environment.get("highland"), 0.f, 1.f));
            if (influence > 0.f)
            {
                // Climate favors clustered, broken, layered, and ragged clouds without excluding other skies.
                std::array<float, 8> climateWeights{};
                float totalWeight = 0.f;
                for (std::size_t i = 0; i < weights.size(); ++i)
                {
                    const bool favored = i == 1 || i == 2 || i == 4 || i == 6;
                    climateWeights[i] = weights[i] * (favored ? 1.f + influence : 1.f);
                    totalWeight += climateWeights[i];
                }
                float selection = static_cast<float>(identity >> 8u) / 16777216.f * totalWeight;
                slot = 0;
                while (slot + 1 < climateWeights.size() && selection >= climateWeights[slot])
                    selection -= climateWeights[slot++];
            }
        }
        else if (target == weatherIndex("Foggy") && (humidEnvironment > 0.4f || dawnCooling > 0.5f))
            slot = contextualSlot({ 0u, 4u, 5u, 6u });
        else if (target == weatherIndex("Overcast") && (wetOrigin || retainedMoisture > 0.55f))
            slot = contextualSlot({ 0u, 2u, 3u, 4u });
        else if (target == weatherIndex("Rain") && origin == weatherIndex("Thunderstorm"))
            slot = contextualSlot({ 0u, 1u, 4u, 7u });
        else if (target == weatherIndex("Thunderstorm") && wetOrigin)
            slot = contextualSlot({ 1u, 2u, 3u, 6u });
        else if ((target == weatherIndex("Snow") || target == weatherIndex("Blizzard"))
            && environment.get("highland") > 0.55f)
            slot = contextualSlot({ 0u, 2u, 4u, 6u });

        identity = (identity & ~7u) | slot;
        return identity == 0 ? 8u | slot : identity;
    }

    MWRender::AtmosphereState AtmospherePalette::getState(std::string_view currentWeather, std::string_view nextWeather,
        float weatherTransition, std::uint32_t currentScenarioIdentity, std::uint32_t nextScenarioIdentity) const
    {
        const float currentWeight = usesScattering(currentWeather) ? 1.f : 0.f;
        const float nextWeight = nextWeather.empty() ? currentWeight : (usesScattering(nextWeather) ? 1.f : 0.f);
        const float transition = std::clamp(weatherTransition, 0.f, 1.f);
        const float weatherWeight = currentWeight * (1.f - transition) + nextWeight * transition;
        const std::size_t currentWeatherIndex = weatherIndex(currentWeather);
        const std::size_t nextWeatherIndex = nextWeather.empty() ? currentWeatherIndex : weatherIndex(nextWeather);
        const VolumeProfile currentVolume = realizeCloudScenario(
            mImpl->mVolumes[currentWeatherIndex], mImpl->mCloudScenarios[currentWeatherIndex], currentScenarioIdentity);
        const VolumeProfile nextVolume = nextWeather.empty()
            ? currentVolume
            : realizeCloudScenario(
                mImpl->mVolumes[nextWeatherIndex], mImpl->mCloudScenarios[nextWeatherIndex], nextScenarioIdentity);
        const float presentation = smoothTransition(transition);
        const VolumeProfile volume = blend(currentVolume, nextVolume, presentation);

        MWRender::AtmosphereState result;
        const Palette selected = mImpl->colors();
        result.mInScattering = selected.mInScattering;
        result.mOutScattering = selected.mOutScattering;
        result.mWeatherWeight = weatherWeight * weatherWeight;
        result.mEnabled = true;
        const VolumeModifiers& environment = mImpl->mAppliedVolumes;
        const auto makeVolumeState = [&](const VolumeProfile& source, std::uint32_t scenarioIdentity) {
            VolumeProfile state = source;
            state.mCloudScenarioIdentity = scenarioIdentity;
            state.mCloudDensity *= environment.mCloudDensityMultiplier;
            state.mCloudBaseAltitudeWorldUnits += environment.mCloudAltitudeOffsetWorldUnits;
            state.mCloudThicknessWorldUnits *= environment.mCloudThicknessMultiplier;
            const float lowInfluence = source.mLowCloudEnvironmentInfluence;
            const float lowOccurrence = std::lerp(1.f, environment.mLowCloudOccurrenceMultiplier, lowInfluence);
            state.mLowCloudCoverage = std::clamp(state.mLowCloudCoverage * lowOccurrence, 0.f, 1.f);
            state.mEnvironmentalCloudLayer.mCoverageAmount *= lowOccurrence;
            state.mLowCloudDensity *= std::lerp(1.f, environment.mLowCloudDensityMultiplier, lowInfluence);
            state.mLowCloudBaseAltitudeWorldUnits += environment.mLowCloudAltitudeOffsetWorldUnits * lowInfluence;
            state.mLowCloudThicknessWorldUnits
                *= std::lerp(1.f, environment.mLowCloudThicknessMultiplier, lowInfluence);
            state.mTerrainReferenceHeightWorldUnits = mImpl->mAppliedTerrainHeightWorldUnits;
            state.mTerrainSampleCenterWorldUnits = mImpl->mAppliedTerrainSampleCenterWorldUnits;
            state.mTerrainGradient = mImpl->mAppliedTerrainGradient;
            state.mValleyDepthWorldUnits = mImpl->mAppliedValleyDepthWorldUnits;
            state.mMistDensity *= environment.mMistDensityMultiplier;
            state.mMistBaseAltitudeWorldUnits += environment.mMistAltitudeOffsetWorldUnits;
            state.mMistThicknessWorldUnits *= environment.mMistThicknessMultiplier;
            state.mFogDensity *= environment.mMistDensityMultiplier;
            state.mFogBaseAltitudeWorldUnits += environment.mMistAltitudeOffsetWorldUnits;
            state.mFogThicknessWorldUnits *= environment.mMistThicknessMultiplier;
            return state;
        };
        result.mVolumes = makeVolumeState(volume, presentation < 0.5f ? currentScenarioIdentity : nextScenarioIdentity);
        result.mOutgoingCloudVolumes = makeVolumeState(currentVolume, currentScenarioIdentity);
        result.mIncomingCloudVolumes = makeVolumeState(nextVolume, nextScenarioIdentity);
        result.mCloudScenarioTransition = nextWeather.empty() ? 0.f : presentation;
        return result;
    }
}
