#ifndef OPENMW_COMPONENTS_SCENEUTIL_SHADOWCASCADE_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_SHADOWCASCADE_HPP

#include <span>
#include <vector>

#include <osg/Vec3d>

namespace SceneUtil
{
    /// Select the distance growth, texture resolution, and caster retention policy for horizon cascades.
    enum class DistantShadowQuality
    {
        Low,
        Medium,
        High,
        Ultra,
    };

    /// Define stable depth partitions and transition coverage for one directional-light shadow set.
    struct ShadowCascadePlanSettings
    {
        double mNearDistance = 1.0;
        double mDetailedDistance = 8192.0;
        double mVisibleDistance = 8192.0;
        double mSplitLogarithmicRatio = 0.5;
        double mSplitBias = 0.0;
        double mTransitionFraction = 0.08;
        unsigned int mDetailedCascadeCount = 3;
        unsigned int mMaximumCascadeCount = 8;
        bool mExtendToVisibleDistance = false;
        DistantShadowQuality mDistantQuality = DistantShadowQuality::Medium;
    };

    /// Describe one nominal cascade interval, its overlap coverage, and relative storage cost.
    struct ShadowCascade
    {
        double mNearDistance = 0.0;
        double mFarDistance = 0.0;
        double mRenderNearDistance = 0.0;
        double mRenderFarDistance = 0.0;
        double mTransitionStartDistance = 0.0;
        double mTransitionEndDistance = 0.0;
        unsigned int mResolutionDivisor = 1;
        float mMinimumCasterPixels = 0.f;
        bool mDistant = false;
    };

    /// Describe an orientation-independent receiver footprint in world space.
    struct StableShadowFootprint
    {
        osg::Vec3d mCenter;
        double mRadius = 0.0;
    };

    /// Describe a world-anchored directional-light frame whose roll is independent of the viewing camera.
    struct StableDirectionalLightBasis
    {
        osg::Vec3d mSide;
        osg::Vec3d mUp;
    };

    /// Describe per-cascade receiver filtering and normal-offset compensation.
    struct ShadowCascadeSampling
    {
        float mFilterRadiusScale = 1.f;
        float mNormalOffsetScale = 1.f;
    };

    /// Build monotonic detailed partitions and optional progressively cheaper visible-distance levels.
    std::vector<ShadowCascade> makeShadowCascadePlan(const ShadowCascadePlanSettings& settings);

    /// Restrict distant shadow traversal to configured world casters while preserving full traversal nearby.
    unsigned int makeShadowCasterTraversalMask(
        unsigned int configuredCasterMask, unsigned int distantWorldMask, bool distant);

    /// Bound a frustum slice with a quantized sphere so camera rotation cannot resize its projection.
    StableShadowFootprint makeStableShadowFootprint(
        std::span<const osg::Vec3d> receiverPoints, double guardWorldUnits, double radiusQuantum);

    /// Build a continuous daylight-hemisphere basis so camera rotation cannot rotate the shadow texel grid.
    StableDirectionalLightBasis makeStableDirectionalLightBasis(const osg::Vec3d& lightDirection);

    /// Round a footprint's world texel size to a shared power-of-two grid for nested cascade alignment.
    double makeNestedShadowTexelSize(
        double footprintRadius, unsigned int textureResolution, double minimumWorldTexelSize);

    /// Keep world-space softness bounded and compensate bias as cascade texels become coarser.
    ShadowCascadeSampling makeShadowCascadeSampling(
        double referenceWorldUnitsPerTexel, double cascadeWorldUnitsPerTexel);

    /// Counter camera-relative translation against a world-origin light-space texel grid without quantizing light
    /// motion.
    osg::Vec3d alignShadowFootprintToWorldTexels(const osg::Vec3d& viewRelativeLightSpaceCenter,
        const osg::Vec3d& worldOriginLightSpaceCenter, double footprintRadius, unsigned int textureResolution);
}

#endif
