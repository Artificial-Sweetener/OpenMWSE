#include "diagnosticrenderconfiguration.hpp"

#include <components/settings/values.hpp>

#include <iomanip>
#include <sstream>

namespace OMW
{
    DiagnosticRenderConfiguration captureDiagnosticRenderConfiguration()
    {
        const auto& shadows = Settings::shadows();
        return {
            .mResolutionWidth = Settings::video().mResolutionX,
            .mResolutionHeight = Settings::video().mResolutionY,
            .mAntialiasing = Settings::video().mAntialiasing,
            .mNearClip = Settings::camera().mNearClip,
            .mViewingDistance = Settings::camera().mViewingDistance,
            .mFieldOfView = Settings::camera().mFieldOfView,
            .mReverseZ = Settings::camera().mReverseZ,
            .mAtmosphericEffects = Settings::shaders().mAtmosphericEffects,
            .mAtmosphericQuality = Settings::shaders().mAtmosphericQuality,
            .mShadowsEnabled = shadows.mEnableShadows,
            .mShadowMapCount = shadows.mNumberOfShadowMaps,
            .mMaximumShadowMapDistance = shadows.mMaximumShadowMapDistance,
            .mShadowFadeStart = shadows.mShadowFadeStart,
            .mShadowSplitRatio = shadows.mSplitPointUniformLogarithmicRatio,
            .mShadowSplitBias = shadows.mSplitPointBias,
            .mShadowSceneBounds = shadows.mComputeSceneBounds,
            .mShadowMapResolution = shadows.mShadowMapResolution,
            .mMinimumShadowNearFarRatio = shadows.mMinimumLispsmNearFarRatio,
            .mShadowPolygonOffsetFactor = shadows.mPolygonOffsetFactor,
            .mShadowPolygonOffsetUnits = shadows.mPolygonOffsetUnits,
            .mShadowNormalOffsetDistance = shadows.mNormalOffsetDistance,
            .mUseFrontFaceCulling = shadows.mUseFrontFaceCulling,
            .mActorShadows = shadows.mActorShadows,
            .mPlayerShadows = shadows.mPlayerShadows,
            .mTerrainShadows = shadows.mTerrainShadows,
            .mObjectShadows = shadows.mObjectShadows,
            .mIndoorShadows = shadows.mEnableIndoorShadows,
        };
    }

    std::string serializeDiagnosticRenderConfiguration(const DiagnosticRenderConfiguration& configuration)
    {
        const auto boolean = [](bool value) { return value ? "true" : "false"; };
        std::ostringstream output;
        output << "video:\n  resolution: [" << configuration.mResolutionWidth << ", " << configuration.mResolutionHeight
               << "]\n  antialiasing: " << configuration.mAntialiasing
               << "\ncamera:\n  near-clip: " << configuration.mNearClip
               << "\n  viewing-distance: " << configuration.mViewingDistance
               << "\n  field-of-view: " << configuration.mFieldOfView
               << "\n  reverse-z: " << boolean(configuration.mReverseZ)
               << "\nshaders:\n  atmospheric-effects: " << boolean(configuration.mAtmosphericEffects)
               << "\n  atmospheric-quality: " << configuration.mAtmosphericQuality
               << "\nshadows:\n  enabled: " << boolean(configuration.mShadowsEnabled)
               << "\n  map-count: " << configuration.mShadowMapCount
               << "\n  maximum-distance: " << configuration.mMaximumShadowMapDistance
               << "\n  fade-start: " << configuration.mShadowFadeStart
               << "\n  split-ratio: " << configuration.mShadowSplitRatio
               << "\n  split-bias: " << configuration.mShadowSplitBias
               << "\n  scene-bounds: " << std::quoted(configuration.mShadowSceneBounds)
               << "\n  map-resolution: " << configuration.mShadowMapResolution
               << "\n  minimum-near-far-ratio: " << configuration.mMinimumShadowNearFarRatio
               << "\n  polygon-offset-factor: " << configuration.mShadowPolygonOffsetFactor
               << "\n  polygon-offset-units: " << configuration.mShadowPolygonOffsetUnits
               << "\n  normal-offset-distance: " << configuration.mShadowNormalOffsetDistance
               << "\n  front-face-culling: " << boolean(configuration.mUseFrontFaceCulling)
               << "\n  actor-casters: " << boolean(configuration.mActorShadows)
               << "\n  player-caster: " << boolean(configuration.mPlayerShadows)
               << "\n  terrain-casters: " << boolean(configuration.mTerrainShadows)
               << "\n  object-casters: " << boolean(configuration.mObjectShadows)
               << "\n  indoor: " << boolean(configuration.mIndoorShadows) << '\n';
        return output.str();
    }
}
