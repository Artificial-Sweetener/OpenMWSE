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
            if (settings.mFog && !settings.mShadowResolution)
                throw std::invalid_argument("fog distance diagnostics require a shadow resolution in version 3");
            if (settings.mShadowResolution
                && !Settings::parseShadowResolution(
                    std::to_string(*settings.mShadowResolution), std::numeric_limits<short>::max()))
                throw std::invalid_argument("shadow resolution must fit a positive renderer texture dimension");
            if (!std::isfinite(settings.mView) || settings.mView <= 0 || !std::isfinite(settings.mGrass)
                || settings.mGrass < 0 || !std::isfinite(settings.mShadow))
                throw std::invalid_argument(
                    "distance settings require finite world units, positive view distance, and non-negative grass "
                    "distance");
            if (settings.mFog
                && (!std::isfinite(settings.mFog->mStart) || !std::isfinite(settings.mFog->mEnd)
                    || settings.mFog->mStart < 0 || !(settings.mFog->mEnd > settings.mFog->mStart)))
                throw std::invalid_argument("fog distances must be finite, non-negative, and strictly ordered");
        }
    }

    DistanceSettings deserializeDistanceSettings(std::string_view text)
    {
        if (text.size() > 4096)
            throw std::invalid_argument("distance settings payload exceeds 4096 bytes");
        const YAML::Node node = YAML::Load(std::string(text));
        const int version = node["distance-version"].as<int>();
        if (!node.IsMap() || (version != 1 && version != 2 && version != 3)
            || node.size()
                != (version == 1       ? 5
                        : version == 2 ? 6
                                       : 11))
            throw std::invalid_argument("distance settings require version 1, 2, or 3 and every versioned field");
        DistanceSettings result{ node["view-world-units"].as<float>(), node["grass-world-units"].as<float>(),
            node["shadow-world-units"].as<float>(), node["use-cell-editor"].as<bool>(), std::nullopt, std::nullopt };
        if (version >= 2)
            result.mShadowResolution = node["shadow-resolution"].as<int>();
        if (version == 3)
            result.mFog = FogDistanceSettings{ node["use-distant-fog"].as<bool>(),
                node["fog-start-world-units"].as<float>(), node["fog-end-world-units"].as<float>(),
                node["fog-start-locked"].as<bool>(), node["fog-end-locked"].as<bool>() };
        validate(result);
        return result;
    }

    std::string serializeDistanceSettings(const DistanceSettings& settings)
    {
        validate(settings);
        YAML::Node node;
        node["distance-version"] = settings.mFog ? 3 : settings.mShadowResolution ? 2 : 1;
        if (settings.mShadowResolution)
            node["shadow-resolution"] = *settings.mShadowResolution;
        node["view-world-units"] = settings.mView;
        node["grass-world-units"] = settings.mGrass;
        node["shadow-world-units"] = settings.mShadow;
        node["use-cell-editor"] = settings.mUseCellEditor;
        if (settings.mFog)
        {
            node["use-distant-fog"] = settings.mFog->mEnabled;
            node["fog-start-world-units"] = settings.mFog->mStart;
            node["fog-end-world-units"] = settings.mFog->mEnd;
            node["fog-start-locked"] = settings.mFog->mStartLocked;
            node["fog-end-locked"] = settings.mFog->mEndLocked;
        }
        return YAML::Dump(node);
    }
}
