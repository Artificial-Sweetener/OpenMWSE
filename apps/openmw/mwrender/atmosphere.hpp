#ifndef OPENMW_MWRENDER_ATMOSPHERE_H
#define OPENMW_MWRENDER_ATMOSPHERE_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <osg/Vec2d>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/sceneutil/cloudadvection.hpp>
#include <components/sceneutil/cloudlayer.hpp>

namespace MWRender
{
    /// Returns the internal atmosphere buffer scale for a user-facing quality level.
    inline float getAtmosphereVolumeResolutionScale(int quality)
    {
        if (quality <= 0)
            return 0.25f;
        if (quality == 1)
            return 0.375f;
        return 0.5f;
    }

    /// Defines weather-controlled cloud, mist, and shaft properties in world units.
    struct AtmosphereVolumeState
    {
        float mCloudDeckCoverage = 0.f;
        float mCloudDensity = 0.f;
        float mCloudBaseAltitudeWorldUnits = 55000.f;
        float mCloudThicknessWorldUnits = 18000.f;
        std::uint32_t mCloudScenarioIdentity = 0;
        float mCloudTerrainFollowing = 0.f;
        float mCloudTerrainClearanceWorldUnits = 0.f;
        float mLowCloudCoverage = 0.f;
        float mLowCloudDensity = 0.f;
        float mLowCloudBaseAltitudeWorldUnits = 4000.f;
        float mLowCloudThicknessWorldUnits = 8000.f;
        float mLowCloudEnvironmentInfluence = 1.f;
        float mTerrainReferenceHeightWorldUnits = 0.f;
        osg::Vec2f mTerrainSampleCenterWorldUnits;
        osg::Vec2f mTerrainGradient;
        float mValleyDepthWorldUnits = 0.f;
        float mMistDensity = 0.f;
        float mMistBaseAltitudeWorldUnits = 800.f;
        float mMistThicknessWorldUnits = 4500.f;
        float mShaftIntensity = 0.f;
        float mFogDensity = 0.f;
        float mFogBaseAltitudeWorldUnits = 0.f;
        float mFogThicknessWorldUnits = 6000.f;
        float mFogAnisotropy = 0.45f;
        SceneUtil::CloudLayer mCloudLayer;
        SceneUtil::CloudLayer mEnvironmentalCloudLayer;
    };

    /// Convert a persistent episode identity to a deterministic procedural-field phase in radians.
    inline float getCloudScenarioFieldPhase(std::uint32_t identity)
    {
        constexpr float fullTurn = 6.28318530718f;
        if (identity == 0)
            return 0.f;
        identity ^= identity >> 16u;
        identity *= 0x7feb352du;
        identity ^= identity >> 15u;
        identity *= 0x846ca68bu;
        identity ^= identity >> 16u;
        return static_cast<float>(identity >> 8u) * (fullTurn / 16777216.f);
    }

    /// Carries the shared scene-linear light sources consumed by every atmospheric and surface-lighting path.
    struct AtmosphereLightingState
    {
        osg::Vec3f mSolarRadiance{ 1.f, 1.f, 1.f };
        osg::Vec3f mSkyIrradiance{ 1.f, 1.f, 1.f };
        osg::Vec3f mGroundIrradiance{ 1.f, 1.f, 1.f };
    };

    /// Reserves canonical optical inputs for a later weatherborne medium that must inherit atmosphere lighting.
    struct AtmosphereSuspendedMediumState
    {
        osg::Vec3f mExtinctionPerWorldUnit{ 0.f, 0.f, 0.f };
        osg::Vec3f mSingleScatteringAlbedo{ 1.f, 1.f, 1.f };
        float mAnisotropy = 0.f;
        float mBaseAltitudeWorldUnits = 0.f;
        float mThicknessWorldUnits = 1.f;
        float mTerrainFollowing = 0.f;
    };

    /// Captures the frame-varying radiance inputs that determine atmosphere-history compatibility.
    struct AtmosphereHistoryLightingState
    {
        osg::Vec3f mSolarRadiance{ 1.f, 1.f, 1.f };
        osg::Vec3f mSkyIrradiance{ 1.f, 1.f, 1.f };
        osg::Vec3f mGroundIrradiance{ 1.f, 1.f, 1.f };
        osg::Vec3f mInScattering{ 0.25f, 0.38f, 0.48f };
        osg::Vec3f mOutScattering{ 0.07f, 0.36f, 0.76f };
        osg::Vec3f mSkyColor{ 1.f, 1.f, 1.f };
        osg::Vec3f mFogColor{ 1.f, 1.f, 1.f };
        osg::Vec3f mSunDirection{ 0.f, 0.f, 1.f };
    };

    /// Converts consecutive atmosphere lighting states into a bounded temporal-history confidence.
    class AtmosphereHistoryLightingController
    {
    public:
        /// Update the retained lighting state and return zero for discontinuities or first-frame history.
        float update(const AtmosphereHistoryLightingState& current, bool enabled)
        {
            if (!enabled || !isFinite(current))
            {
                reset();
                return 0.f;
            }

            if (!mInitialized)
            {
                mPrevious = current;
                mInitialized = true;
                return 0.f;
            }

            float change = relativeDifference(current.mSolarRadiance, mPrevious.mSolarRadiance);
            change = std::max(change, relativeDifference(current.mSkyIrradiance, mPrevious.mSkyIrradiance));
            change = std::max(change, relativeDifference(current.mGroundIrradiance, mPrevious.mGroundIrradiance));
            change = std::max(change, relativeDifference(current.mInScattering, mPrevious.mInScattering));
            change = std::max(change, relativeDifference(current.mOutScattering, mPrevious.mOutScattering));
            change = std::max(change, relativeDifference(current.mSkyColor, mPrevious.mSkyColor));
            change = std::max(change, relativeDifference(current.mFogColor, mPrevious.mFogColor));
            osg::Vec3f currentSunDirection = current.mSunDirection;
            osg::Vec3f previousSunDirection = mPrevious.mSunDirection;
            currentSunDirection.normalize();
            previousSunDirection.normalize();
            const float sunAngularChange = std::sqrt(
                std::max(0.f, 2.f * (1.f - std::clamp(currentSunDirection * previousSunDirection, -1.f, 1.f))));
            change = std::max(change, sunAngularChange * 0.5f);
            mPrevious = current;

            constexpr float fullHistoryChange = 0.015f;
            constexpr float rejectedHistoryChange = 0.12f;
            const float normalizedChange
                = std::clamp((change - fullHistoryChange) / (rejectedHistoryChange - fullHistoryChange), 0.f, 1.f);
            const float smoothChange = normalizedChange * normalizedChange * (3.f - 2.f * normalizedChange);
            return 1.f - smoothChange;
        }

        /// Forget the retained state so the next enabled frame cannot consume incompatible history.
        void reset() { mInitialized = false; }

    private:
        /// Return a scale-independent vector difference with a stable dark-state denominator.
        static float relativeDifference(const osg::Vec3f& current, const osg::Vec3f& previous)
        {
            return (current - previous).length() / std::max({ current.length(), previous.length(), 0.05f });
        }

        /// Return whether every component required by temporal lighting validation is finite.
        static bool isFinite(const AtmosphereHistoryLightingState& state)
        {
            const auto finiteVector = [](const osg::Vec3f& value) {
                return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
            };
            return finiteVector(state.mSolarRadiance) && finiteVector(state.mSkyIrradiance)
                && finiteVector(state.mGroundIrradiance) && finiteVector(state.mInScattering)
                && finiteVector(state.mOutScattering) && finiteVector(state.mSkyColor) && finiteVector(state.mFogColor)
                && finiteVector(state.mSunDirection) && state.mSunDirection.length2() > 1e-8f;
        }

        AtmosphereHistoryLightingState mPrevious;
        bool mInitialized = false;
    };

    /// Build the canonical scene-linear atmosphere lighting inputs from the active weather colors.
    inline AtmosphereLightingState makeAtmosphereLightingState(
        const osg::Vec4f& sunColor, const osg::Vec4f& ambientColor, const osg::Vec4f& fogColor)
    {
        const osg::Vec3f skyIrradiance(ambientColor.x(), ambientColor.y(), ambientColor.z());
        return { osg::Vec3f(sunColor.x(), sunColor.y(), sunColor.z()), skyIrradiance,
            skyIrradiance * 0.65f + osg::Vec3f(fogColor.x(), fogColor.y(), fogColor.z()) * 0.35f };
    }

    /// Carries the integrated world-space cloud displacement and its exact current-frame delta.
    struct CloudAdvectionState
    {
        osg::Vec2f mOffsetWorldUnits{ 0.f, 0.f };
        osg::Vec2f mFrameDeltaWorldUnits{ 0.f, 0.f };
    };

    /// Integrates changing weather velocity without retroactively moving the procedural cloud field.
    class CloudAdvectionController
    {
    public:
        /// Advance the cloud field from the active weather speed and game-time rate, or publish zero motion while
        /// paused.
        void update(float elapsedSeconds, bool paused, float cloudSpeed, float gameTimeScale)
        {
            mState.mFrameDeltaWorldUnits.set(0.f, 0.f);
            if (paused || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.f || !std::isfinite(cloudSpeed)
                || cloudSpeed <= 0.f || !std::isfinite(gameTimeScale) || gameTimeScale <= 0.f)
                return;

            constexpr double defaultGameTimeScale = 30.0;
            constexpr double worldUnitsPerCloudSpeedSecond = 64.0;
            const double gameTimeRate = static_cast<double>(gameTimeScale) / defaultGameTimeScale;
            const double speedWorldUnitsPerSecond
                = worldUnitsPerCloudSpeedSecond * static_cast<double>(cloudSpeed) * gameTimeRate;
            const osg::Vec2d frameDelta = SceneUtil::cloudCoverageWindDirection()
                * (speedWorldUnitsPerSecond * static_cast<double>(elapsedSeconds));
            mAccumulatedOffsetWorldUnits += frameDelta;
            mState.mOffsetWorldUnits.set(static_cast<float>(mAccumulatedOffsetWorldUnits.x()),
                static_cast<float>(mAccumulatedOffsetWorldUnits.y()));
            mState.mFrameDeltaWorldUnits.set(static_cast<float>(frameDelta.x()), static_cast<float>(frameDelta.y()));
        }

        /// Return the canonical cloud displacement shared by volume, reflection, shadow, and reprojection paths.
        const CloudAdvectionState& getState() const { return mState; }

    private:
        osg::Vec2d mAccumulatedOffsetWorldUnits{ 0.0, 0.0 };
        CloudAdvectionState mState;
    };

    /// Converts the engine sunlight vector to the legacy sky-dome sun position.
    inline osg::Vec3f getRenderedExteriorSunPosition(const osg::Vec3f& sunlightDirection)
    {
        osg::Vec3f position = -sunlightDirection;
        position.z() = 400.f - std::abs(position.x());
        return position;
    }

    /// Restores the physical below-horizon sun trajectory hidden by Morrowind's rendered sun bounce.
    inline osg::Vec3f getScatteringSunDirection(osg::Vec3f renderedSunDirection, bool isDay)
    {
        // Morrowind bounces the rendered sun above the horizon at night. MGE XE restores its physical trajectory here.
        if (!isDay)
            renderedSunDirection.z() = -std::abs(renderedSunDirection.z());
        renderedSunDirection.normalize();
        return renderedSunDirection;
    }

    /// Describes the authoritative atmosphere state for one rendered weather frame.
    struct AtmosphereState
    {
        osg::Vec3f mInScattering{ 0.25f, 0.38f, 0.48f };
        osg::Vec3f mOutScattering{ 0.07f, 0.36f, 0.76f };
        float mWeatherWeight = 0.f;
        bool mEnabled = false;
        AtmosphereVolumeState mVolumes;
        /// Retain both endpoint fields while an authoritative weather transition is active.
        AtmosphereVolumeState mOutgoingCloudVolumes;
        AtmosphereVolumeState mIncomingCloudVolumes;
        float mCloudScenarioTransition = 0.f;
        AtmosphereSuspendedMediumState mSuspendedMedium;
        AtmosphereLightingState mLighting;
        osg::Vec4f mSunDiscColor{ 1.f, 1.f, 1.f, 0.f };
    };
}

#endif
