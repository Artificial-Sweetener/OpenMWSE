#include "watersettings.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace Diagnostics
{
    namespace
    {
        /// Keep diagnostic input within the same domain exposed by the in-game slider.
        void validate(const WaterSettings& settings)
        {
            if (!std::isfinite(settings.mGeometricWaveHeight) || settings.mGeometricWaveHeight < 0.f
                || settings.mGeometricWaveHeight > 250.f
                || settings.mGeometricWaveHeight != std::round(settings.mGeometricWaveHeight))
                throw std::invalid_argument(
                    "geometric wave height must be a whole number between 0 and 250 Morrowind world units");
            if (!std::isfinite(settings.mMeshGeometricWaveHeight) || settings.mMeshGeometricWaveHeight < 0.f
                || settings.mMeshGeometricWaveHeight > 250.f
                || settings.mMeshGeometricWaveHeight != std::round(settings.mMeshGeometricWaveHeight))
                throw std::invalid_argument(
                    "mesh geometric wave height must be a whole number between 0 and 250 Morrowind world units");
            if (!std::isfinite(settings.mCausticsStrength) || settings.mCausticsStrength < 0.f
                || settings.mCausticsStrength > 2.f)
                throw std::invalid_argument("caustics strength must be between 0 and 2");
            if (settings.mInteriorLocalLights < 0 || settings.mInteriorLocalLights > 4
                || settings.mExteriorLocalLights < 0 || settings.mExteriorLocalLights > 4)
                throw std::invalid_argument("local water-light budgets must be between 0 and 4");
            constexpr std::array<std::string_view, 4> appearances{ "openmw", "mge-xe", "enhanced", "vanilla" };
            if (std::find(appearances.begin(), appearances.end(), settings.mAppearance) == appearances.end())
                throw std::invalid_argument("water appearance must be openmw, mge-xe, enhanced, or vanilla");
        }
    }

    WaterSettings deserializeWaterSettings(std::string_view text)
    {
        if (text.size() > 4096)
            throw std::invalid_argument("water settings payload exceeds 4096 bytes");
        const YAML::Node node = YAML::Load(std::string(text));
        if (!node.IsMap())
            throw std::invalid_argument("water settings must be a map");
        const int version = node["water-version"].as<int>();
        if (version < 1 || version > 8)
            throw std::invalid_argument("water settings require a version from 1 through 8");
        const std::size_t expectedSize = version == 1 ? 2
            : version == 2                            ? 4
            : version == 3                            ? 5
            : version == 4                            ? 7
            : version == 5                            ? 6
            : version == 6                            ? 7
            : version == 7                            ? 8
                                                      : 9;
        if (node.size() != expectedSize)
            throw std::invalid_argument("water settings contain missing or unknown fields");
        WaterSettings result;
        result.mGeometricWaveHeight = node["geometric-wave-height"].as<float>();
        if (version >= 2)
        {
            result.mCaustics = node["caustics"].as<bool>();
            result.mCausticsStrength = node["caustics-strength"].as<float>();
        }
        if (version == 3 || version == 4)
            static_cast<void>(node["reflected-caustics"].as<bool>());
        if (version >= 4)
        {
            result.mInteriorLocalLights = node["interior-local-lights"].as<int>();
            result.mExteriorLocalLights = node["exterior-local-lights"].as<int>();
        }
        if (version >= 6)
            result.mAppearance = node["appearance"].as<std::string>();
        if (version >= 7)
            result.mMeshWater = node["mesh-water"].as<bool>();
        if (version >= 8)
            result.mMeshGeometricWaveHeight = node["mesh-geometric-wave-height"].as<float>();
        validate(result);
        return result;
    }

    std::string serializeWaterSettings(const WaterSettings& settings)
    {
        validate(settings);
        YAML::Node node;
        node["water-version"] = 8;
        node["geometric-wave-height"] = settings.mGeometricWaveHeight;
        node["caustics"] = settings.mCaustics;
        node["caustics-strength"] = settings.mCausticsStrength;
        node["interior-local-lights"] = settings.mInteriorLocalLights;
        node["exterior-local-lights"] = settings.mExteriorLocalLights;
        node["appearance"] = settings.mAppearance;
        node["mesh-water"] = settings.mMeshWater;
        node["mesh-geometric-wave-height"] = settings.mMeshGeometricWaveHeight;
        return YAML::Dump(node);
    }
}
