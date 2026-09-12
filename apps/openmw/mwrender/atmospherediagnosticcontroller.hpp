#ifndef OPENMW_MWRENDER_ATMOSPHEREDIAGNOSTICCONTROLLER_H
#define OPENMW_MWRENDER_ATMOSPHEREDIAGNOSTICCONTROLLER_H

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include "atmosphere.hpp"

namespace SceneUtil
{
    class StateUpdater;
}

namespace MWRender
{
    /// Select the production atmosphere media retained by an isolated diagnostic scenario.
    struct AtmosphereDiagnosticMedia
    {
        bool mMainClouds = true;
        bool mLowClouds = true;
        bool mMist = true;
        bool mFog = true;
    };

    /// Capture the range, media, and canonical illumination applied during the latest rendered frame.
    struct AtmosphereDiagnosticState
    {
        float mFogStartWorldUnits = 0.f;
        float mFogEndWorldUnits = 0.f;
        float mNearClipWorldUnits = 0.f;
        float mViewDistanceWorldUnits = 0.f;
        bool mEnabled = false;
        AtmosphereVolumeState mVolumes;
        osg::Vec3f mSunDirection{ 0.f, 0.f, 1.f };
        AtmosphereLightingState mLighting;
        osg::Vec4f mSunDiscColor{ 1.f, 1.f, 1.f, 0.f };
        osg::Vec2f mCloudFieldOffsetWorldUnits;
    };

    /// Provide atmosphere values after applying isolated diagnostic policy.
    struct AtmosphereDiagnosticFrame
    {
        float mFogStartWorldUnits = 0.f;
        float mFogEndWorldUnits = 0.f;
        AtmosphereVolumeState mVolumes;
        AtmosphereVolumeState mOutgoingCloudVolumes;
        AtmosphereVolumeState mIncomingCloudVolumes;
    };

    /// Own isolated atmosphere diagnostic policy and its last applied frame state.
    class AtmosphereDiagnosticController
    {
    public:
        /// Bind diagnostic shader modes to the renderer's production state updater.
        explicit AtmosphereDiagnosticController(SceneUtil::StateUpdater& stateUpdater);

        /// Select an inert or capture-only compatibility-shader diagnostic output.
        void setMode(int mode);

        /// Apply a diagnostic medium mask without mutating weather or content state.
        void setMedia(const AtmosphereDiagnosticMedia& media);

        /// Select whether distance-atmosphere fog participates in isolated captures.
        void setDistanceFogEnabled(bool enabled);

        /// Resolve one production atmosphere frame through the selected diagnostic policy.
        AtmosphereDiagnosticFrame resolveFrame(float fogStartWorldUnits, float fogEndWorldUnits,
            float nearClipWorldUnits, float viewDistanceWorldUnits, bool atmosphereEnabled,
            bool volumetricCloudsEnabled, bool atmosphericScatteringEnabled, const AtmosphereState& atmosphere,
            const osg::Vec3f& sunDirection, const osg::Vec2f& cloudFieldOffsetWorldUnits);

        /// Return the atmosphere values applied during the latest resolved frame.
        const AtmosphereDiagnosticState& state() const { return mState; }

    private:
        SceneUtil::StateUpdater& mStateUpdater;
        AtmosphereDiagnosticMedia mMedia;
        AtmosphereDiagnosticState mState;
        bool mDistanceFogEnabled = true;
    };
}

#endif
