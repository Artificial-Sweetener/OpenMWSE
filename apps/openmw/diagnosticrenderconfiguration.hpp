#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICRENDERCONFIGURATION_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICRENDERCONFIGURATION_H

#include <string>

namespace OMW
{
    /// Captures effective render settings needed to qualify a diagnostic replay.
    struct DiagnosticRenderConfiguration
    {
        int mResolutionWidth = 0;
        int mResolutionHeight = 0;
        int mAntialiasing = 0;
        float mNearClip = 0.f;
        float mViewingDistance = 0.f;
        float mFieldOfView = 0.f;
        bool mReverseZ = false;
        bool mAtmosphericEffects = false;
        int mAtmosphericQuality = 0;
        bool mShadowsEnabled = false;
        int mShadowMapCount = 0;
        float mMaximumShadowMapDistance = 0.f;
        float mShadowFadeStart = 0.f;
        float mShadowSplitRatio = 0.f;
        float mShadowSplitBias = 0.f;
        std::string mShadowSceneBounds;
        int mShadowMapResolution = 0;
        float mMinimumShadowNearFarRatio = 0.f;
        float mShadowPolygonOffsetFactor = 0.f;
        float mShadowPolygonOffsetUnits = 0.f;
        float mShadowNormalOffsetDistance = 0.f;
        bool mUseFrontFaceCulling = false;
        bool mActorShadows = false;
        bool mPlayerShadows = false;
        bool mTerrainShadows = false;
        bool mObjectShadows = false;
        bool mIndoorShadows = false;
    };

    /// Snapshot the effective render settings used by the current engine instance.
    DiagnosticRenderConfiguration captureDiagnosticRenderConfiguration();

    /// Serialize an effective render-settings snapshot as stable capsule YAML.
    std::string serializeDiagnosticRenderConfiguration(const DiagnosticRenderConfiguration& configuration);
}

#endif
