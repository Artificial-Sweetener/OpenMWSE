#include "atmospherediagnosticcontroller.hpp"

#include <algorithm>

#include <components/sceneutil/stateupdater.hpp>

namespace MWRender
{
    AtmosphereDiagnosticController::AtmosphereDiagnosticController(SceneUtil::StateUpdater& stateUpdater)
        : mStateUpdater(stateUpdater)
    {
    }

    void AtmosphereDiagnosticController::setMode(int mode)
    {
        mStateUpdater.setAtmosphereDiagnosticMode(mode);
    }

    void AtmosphereDiagnosticController::setMedia(const AtmosphereDiagnosticMedia& media)
    {
        mMedia = media;
    }

    void AtmosphereDiagnosticController::setDistanceFogEnabled(bool enabled)
    {
        mDistanceFogEnabled = enabled;
    }

    AtmosphereDiagnosticFrame AtmosphereDiagnosticController::resolveFrame(float fogStartWorldUnits,
        float fogEndWorldUnits, float nearClipWorldUnits, float viewDistanceWorldUnits, bool atmosphereEnabled,
        bool volumetricCloudsEnabled, bool atmosphericScatteringEnabled, const AtmosphereState& atmosphere,
        const osg::Vec3f& sunDirection, const osg::Vec2f& cloudFieldOffsetWorldUnits)
    {
        if (!mDistanceFogEnabled)
        {
            fogStartWorldUnits = std::max(viewDistanceWorldUnits * 100.f, 1000000.f);
            fogEndWorldUnits = fogStartWorldUnits * 2.f;
        }

        AtmosphereDiagnosticFrame frame{ fogStartWorldUnits, fogEndWorldUnits, atmosphere.mVolumes,
            atmosphere.mOutgoingCloudVolumes, atmosphere.mIncomingCloudVolumes };
        if (!volumetricCloudsEnabled || !mMedia.mMainClouds)
        {
            frame.mVolumes.mCloudDeckCoverage = 0.f;
            frame.mVolumes.mCloudDensity = 0.f;
            frame.mOutgoingCloudVolumes.mCloudDeckCoverage = 0.f;
            frame.mOutgoingCloudVolumes.mCloudDensity = 0.f;
            frame.mIncomingCloudVolumes.mCloudDeckCoverage = 0.f;
            frame.mIncomingCloudVolumes.mCloudDensity = 0.f;
        }
        if (!volumetricCloudsEnabled || !mMedia.mLowClouds)
        {
            frame.mVolumes.mLowCloudCoverage = 0.f;
            frame.mVolumes.mLowCloudDensity = 0.f;
        }
        if (!atmosphericScatteringEnabled || !mMedia.mMist)
            frame.mVolumes.mMistDensity = 0.f;
        if (!atmosphericScatteringEnabled || !mMedia.mFog)
            frame.mVolumes.mFogDensity = 0.f;

        mState = { fogStartWorldUnits, fogEndWorldUnits, nearClipWorldUnits, viewDistanceWorldUnits, atmosphereEnabled,
            frame.mVolumes, sunDirection, atmosphere.mLighting, atmosphere.mSunDiscColor, cloudFieldOffsetWorldUnits };
        return frame;
    }
}
