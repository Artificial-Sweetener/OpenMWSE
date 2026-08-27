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

        osg::Vec3f readVec3(const YAML::Node& node, std::string_view path)
        {
            if (!node.IsSequence() || node.size() != 3)
                throw std::runtime_error("OpenMWSE atmosphere value must contain three numbers: " + std::string(path));
            return { node[0].as<float>(), node[1].as<float>(), node[2].as<float>() };
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

    struct AtmospherePalette::Impl
    {
        explicit Impl(std::istream& stream)
        {
            const YAML::Node root = YAML::Load(stream);
            const YAML::Node presets = root["presets"];
            const YAML::Node regions = root["regions"];
            if (!presets.IsMap() || !regions.IsMap())
                throw std::runtime_error("OpenMWSE atmosphere data must define presets and regions");

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
        Palette mApplied;
        Palette mFrom;
        Palette mTarget;
        std::string mTargetName;
        float mTransition = 1.f;
    };

    AtmospherePalette::AtmospherePalette(const VFS::Manager& vfs)
        : AtmospherePalette(*vfs.get(VFS::Path::NormalizedView("openmwse/atmosphere.json")))
    {
    }

    AtmospherePalette::AtmospherePalette(std::istream& stream)
        : mImpl(std::make_unique<Impl>(stream))
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
        return { mImpl->mApplied.mInScattering, mImpl->mApplied.mOutScattering, weatherWeight * weatherWeight, true };
    }
}
