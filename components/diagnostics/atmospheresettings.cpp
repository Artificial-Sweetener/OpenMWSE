#include "atmospheresettings.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>

namespace Diagnostics
{
    AtmosphereSettings deserializeAtmosphereSettings(std::string_view text)
    {
        if (text.size() > 4096)
            throw std::invalid_argument("atmosphere settings payload exceeds 4096 bytes");
        const YAML::Node node = YAML::Load(std::string(text));
        if (!node.IsMap())
            throw std::invalid_argument("atmosphere settings must be a mapping");
        const int version = node["settings-version"].as<int>();
        if ((version != 1 || node.size() != 6) && (version != 2 || node.size() != 10))
            throw std::invalid_argument("atmosphere settings require version 1 or 2 and every versioned preference");
        AtmosphereSettings result{ node["scattering"].as<bool>(), node["clouds"].as<bool>(), node["quality"].as<int>(),
            node["shadows"].as<bool>(), node["reflections"].as<bool>() };
        if (version == 2)
        {
            result.mAppearance = node["appearance"].as<int>();
            result.mGodrays = node["godrays"].as<bool>();
            result.mSunGlare = node["sun-glare"].as<bool>();
            result.mCloudSunOcclusion = node["cloud-sun-occlusion"].as<bool>();
        }
        if (result.mQuality < 0 || result.mQuality > 3)
            throw std::invalid_argument("atmosphere quality must be within [0, 3]");
        if (result.mAppearance < 0 || result.mAppearance > 1)
            throw std::invalid_argument("atmosphere appearance must be within [0, 1]");
        return result;
    }

    std::string serializeAtmosphereSettings(const AtmosphereSettings& settings)
    {
        if (settings.mQuality < 0 || settings.mQuality > 3)
            throw std::invalid_argument("atmosphere quality must be within [0, 3]");
        if (settings.mAppearance < 0 || settings.mAppearance > 1)
            throw std::invalid_argument("atmosphere appearance must be within [0, 1]");
        YAML::Node node;
        node["settings-version"] = 2;
        node["scattering"] = settings.mScattering;
        node["clouds"] = settings.mClouds;
        node["quality"] = settings.mQuality;
        node["shadows"] = settings.mShadows;
        node["reflections"] = settings.mReflections;
        node["appearance"] = settings.mAppearance;
        node["godrays"] = settings.mGodrays;
        node["sun-glare"] = settings.mSunGlare;
        node["cloud-sun-occlusion"] = settings.mCloudSunOcclusion;
        return YAML::Dump(node);
    }
}
