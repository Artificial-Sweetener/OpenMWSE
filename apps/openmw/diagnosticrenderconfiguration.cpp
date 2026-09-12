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
            .mAtmosphericEffects = Settings::shaders().mAtmosphericScattering || Settings::shaders().mVolumetricClouds,
            .mAtmosphericScattering = Settings::shaders().mAtmosphericScattering,
            .mVolumetricClouds = Settings::shaders().mVolumetricClouds,
            .mCloudShadows = shadows.mCloudShadows,
            .mCloudShadowIntensity = Settings::openmwseShadows().mCloudShadowIntensity,
            .mReflectClouds = Settings::water().mReflectClouds,
            .mAtmosphericQuality = Settings::shaders().mAtmosphericQuality,
            .mAtmosphereAppearance = Settings::shaders().mAtmosphereAppearance,
            .mGodrays = Settings::shaders().mGodrays,
            .mSunGlare = Settings::shaders().mSunGlare,
            .mCloudSunOcclusion = Settings::shaders().mCloudSunOcclusion,
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
            .mShadowQuality = Settings::openmwseShadows().mFilteringQuality,
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
               << "\n  atmospheric-scattering: " << boolean(configuration.mAtmosphericScattering)
               << "\n  volumetric-clouds: " << boolean(configuration.mVolumetricClouds)
               << "\n  cloud-shadows: " << boolean(configuration.mCloudShadows)
               << "\n  reflect-clouds: " << boolean(configuration.mReflectClouds)
               << "\n  atmospheric-quality: " << configuration.mAtmosphericQuality
               << "\n  atmosphere-appearance: " << configuration.mAtmosphereAppearance
               << "\n  godrays: " << boolean(configuration.mGodrays)
               << "\n  sun-glare: " << boolean(configuration.mSunGlare)
               << "\n  cloud-sun-occlusion: " << boolean(configuration.mCloudSunOcclusion)
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
               << "\n  indoor: " << boolean(configuration.mIndoorShadows)
               << "\nopenmwse-shadows:\n  quality: " << std::quoted(configuration.mShadowQuality)
               << "\n  cloud-shadow-intensity: " << configuration.mCloudShadowIntensity << '\n';
        return output.str();
    }
}
