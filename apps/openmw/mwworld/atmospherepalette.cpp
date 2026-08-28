#include "atmospherepalette.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include <osg/Vec4f>
#include <yaml-cpp/yaml.h>

#include <components/vfs/manager.hpp>

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

        /// Store validated per-weather cloud, mist, and shaft controls in world units.
        struct VolumeProfile
        {
            float mCloudCoverage;
            float mCloudDensity;
            float mCloudBaseAltitudeWorldUnits;
            float mCloudThicknessWorldUnits;
            float mMistDensity;
            float mMistBaseAltitudeWorldUnits;
            float mMistThicknessWorldUnits;
            float mShaftIntensity;
        };

        /// Parse and validate one weather volume profile with field-qualified errors.
        VolumeProfile readVolumeProfile(const YAML::Node& source, std::string_view path)
        {
            if (!source.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere data is missing volume profile: " + std::string(path));
            return { readBoundedFloat(source, "cloudCoverage", path, 0.f, 1.f),
                readBoundedFloat(source, "cloudDensity", path, 0.f, 4.f),
                readBoundedFloat(source, "cloudAltitude", path, -10000.f, 200000.f),
                readBoundedFloat(source, "cloudThickness", path, 1.f, 200000.f),
                readBoundedFloat(source, "mistDensity", path, 0.f, 4.f),
                readBoundedFloat(source, "mistAltitude", path, -10000.f, 50000.f),
                readBoundedFloat(source, "mistThickness", path, 1.f, 100000.f),
                readBoundedFloat(source, "shaftIntensity", path, 0.f, 4.f) };
        }

        /// Interpolate weather volume controls during a weather transition.
        VolumeProfile blend(const VolumeProfile& from, const VolumeProfile& to, float factor)
        {
            return { std::lerp(from.mCloudCoverage, to.mCloudCoverage, factor),
                std::lerp(from.mCloudDensity, to.mCloudDensity, factor),
                std::lerp(from.mCloudBaseAltitudeWorldUnits, to.mCloudBaseAltitudeWorldUnits, factor),
                std::lerp(from.mCloudThicknessWorldUnits, to.mCloudThicknessWorldUnits, factor),
                std::lerp(from.mMistDensity, to.mMistDensity, factor),
                std::lerp(from.mMistBaseAltitudeWorldUnits, to.mMistBaseAltitudeWorldUnits, factor),
                std::lerp(from.mMistThicknessWorldUnits, to.mMistThicknessWorldUnits, factor),
                std::lerp(from.mShaftIntensity, to.mShaftIntensity, factor) };
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
            const YAML::Node regions = root["regions"];
            if (!presets.IsMap() || !regions.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere data must define presets and regions");

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

            for (const auto& entry : regions)
            {
                const std::string preset = entry.second.as<std::string>();
                if (!mPresets.contains(preset))
                    throw std::runtime_error("OpenMWSE atmosphere region references an unknown preset: " + preset);
                mRegions.emplace(lowerCase(entry.first.as<std::string>()), preset);
            }

            mApplied = mFrom = mTarget = defaultPreset->second;
            mTargetName = "default";
        }

        void setRegion(std::string_view regionId)
        {
            const auto region = mRegions.find(lowerCase(regionId));
            const std::string nextName = region == mRegions.end() ? "default" : region->second;
            if (nextName == mTargetName)
                return;

            mFrom = mApplied;
            mTarget = mPresets.at(nextName);
            mTargetName = nextName;
            mTransition = 0.f;
        }

        void update(float elapsedSeconds, bool paused, std::span<Weather> weather)
        {
            if (!paused && mTransition < 1.f)
                mTransition = std::min(1.f, mTransition + std::max(0.f, elapsedSeconds) / sRegionTransitionSeconds);
            mApplied = blend(mFrom, mTarget, mTransition);

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
        std::unordered_map<std::string, std::string> mRegions;
        std::array<VolumeProfile, sWeatherNames.size()> mVolumes;
        Palette mApplied;
        Palette mFrom;
        Palette mTarget;
        std::string mTargetName;
        float mTransition = 1.f;
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

    void AtmospherePalette::setRegion(std::string_view regionId)
    {
        mImpl->setRegion(regionId);
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
        const VolumeProfile volume = blend(currentVolume, nextVolume, transition);

        MWRender::AtmosphereState result;
        result.mInScattering = mImpl->mApplied.mInScattering;
        result.mOutScattering = mImpl->mApplied.mOutScattering;
        result.mWeatherWeight = weatherWeight * weatherWeight;
        result.mEnabled = true;
        result.mVolumes = { volume.mCloudCoverage, volume.mCloudDensity, volume.mCloudBaseAltitudeWorldUnits,
            volume.mCloudThicknessWorldUnits, volume.mMistDensity, volume.mMistBaseAltitudeWorldUnits,
            volume.mMistThicknessWorldUnits, volume.mShaftIntensity };
        return result;
    }
}
