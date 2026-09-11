#include "distancesettings.hpp"

#include <components/settings/shadowresolution.hpp>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <stdexcept>

namespace Diagnostics
{
    namespace
    {
        /// Validate the persisted OpenMW domains, including non-positive unlimited shadow limits.
        void validate(const DistanceSettings& settings)
        {
            if (settings.mShadowResolution
                && !Settings::parseShadowResolution(
                    std::to_string(*settings.mShadowResolution), std::numeric_limits<short>::max()))
                throw std::invalid_argument("shadow resolution must fit a positive renderer texture dimension");
            if (!std::isfinite(settings.mView) || settings.mView <= 0 || !std::isfinite(settings.mGrass)
                || settings.mGrass < 0 || !std::isfinite(settings.mShadow))
                throw std::invalid_argument(
                    "distance settings require finite world units, positive view distance, and non-negative grass "
                    "distance");
        }
    }

    DistanceSettings deserializeDistanceSettings(std::string_view text)
    {
        if (text.size() > 4096)
            throw std::invalid_argument("distance settings payload exceeds 4096 bytes");
        const YAML::Node node = YAML::Load(std::string(text));
        const int version = node["distance-version"].as<int>();
        if (!node.IsMap() || (version != 1 && version != 2) || node.size() != (version == 1 ? 5 : 6))
            throw std::invalid_argument(
                "distance settings require version 1 or 2, all three distances and the input editor; version 2 adds "
                "shadow-resolution");
        DistanceSettings result{ node["view-world-units"].as<float>(), node["grass-world-units"].as<float>(),
            node["shadow-world-units"].as<float>(), node["use-cell-editor"].as<bool>() };
        if (version == 2)
            result.mShadowResolution = node["shadow-resolution"].as<int>();
        validate(result);
        return result;
    }

    std::string serializeDistanceSettings(const DistanceSettings& settings)
    {
        validate(settings);
        YAML::Node node;
        node["distance-version"] = settings.mShadowResolution ? 2 : 1;
        if (settings.mShadowResolution)
            node["shadow-resolution"] = *settings.mShadowResolution;
        node["view-world-units"] = settings.mView;
        node["grass-world-units"] = settings.mGrass;
        node["shadow-world-units"] = settings.mShadow;
        node["use-cell-editor"] = settings.mUseCellEditor;
        return YAML::Dump(node);
    }
}
