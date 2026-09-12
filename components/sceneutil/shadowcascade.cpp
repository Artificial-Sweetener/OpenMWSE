#include "shadowcascade.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace SceneUtil
{
    namespace
    {
        /// Centralize the complete storage and culling tradeoff represented by one distant-shadow quality name.
        struct DistantCascadePolicy
        {
            double mDistanceMultiplier;
            unsigned int mFirstResolutionDivisor;
            unsigned int mLaterResolutionDivisor;
            float mInitialMinimumCasterPixels;
            float mMaximumMinimumCasterPixels;
        };

        /// Return bounded progressively more detailed horizon-cascade policy without changing nearby cascades.
        DistantCascadePolicy distantCascadePolicy(DistantShadowQuality quality)
        {
            switch (quality)
            {
                case DistantShadowQuality::Low:
                    return { 4.0, 4, 8, 4.f, 24.f };
                case DistantShadowQuality::Medium:
                    return { 2.0, 2, 4, 2.f, 12.f };
                case DistantShadowQuality::High:
                    return { 2.0, 2, 2, 1.f, 8.f };
                case DistantShadowQuality::Ultra:
                    return { 2.0, 1, 2, 0.5f, 4.f };
            }
            throw std::invalid_argument("unknown distant shadow quality");
        }

        /// Reject invalid internal cascade policy before it can create discontinuous projections.
        void validatePlanSettings(const ShadowCascadePlanSettings& settings)
        {
            const auto finite = [](double value) { return std::isfinite(value); };
            if (!finite(settings.mNearDistance) || settings.mNearDistance <= 0.0)
                throw std::invalid_argument("shadow cascade near distance must be finite and positive");
            if (!finite(settings.mDetailedDistance) || settings.mDetailedDistance <= settings.mNearDistance)
                throw std::invalid_argument("shadow cascade detailed distance must exceed its near distance");
            if (!finite(settings.mVisibleDistance) || settings.mVisibleDistance < settings.mDetailedDistance)
                throw std::invalid_argument("shadow cascade visible distance must cover its detailed distance");
            if (!finite(settings.mSplitLogarithmicRatio) || settings.mSplitLogarithmicRatio < 0.0
                || settings.mSplitLogarithmicRatio > 1.0)
                throw std::invalid_argument("shadow cascade logarithmic ratio must be within [0, 1]");
            if (!finite(settings.mSplitBias))
                throw std::invalid_argument("shadow cascade split bias must be finite");
            if (!finite(settings.mTransitionFraction) || settings.mTransitionFraction < 0.0
                || settings.mTransitionFraction > 0.25)
                throw std::invalid_argument("shadow cascade transition fraction must be within [0, 0.25]");
            if (settings.mDetailedCascadeCount == 0 || settings.mMaximumCascadeCount == 0
                || settings.mDetailedCascadeCount > settings.mMaximumCascadeCount)
                throw std::invalid_argument("shadow cascade counts must be nonzero and ordered");
        }

        /// Return a practical split while retaining enough range for every remaining cascade.
        double detailedSplit(const ShadowCascadePlanSettings& settings, unsigned int index, double previous)
        {
            const double fraction = static_cast<double>(index) / settings.mDetailedCascadeCount;
            const double logarithmic
                = settings.mNearDistance * std::pow(settings.mDetailedDistance / settings.mNearDistance, fraction);
            const double uniform
                = settings.mNearDistance + (settings.mDetailedDistance - settings.mNearDistance) * fraction;
            const double requested
                = std::lerp(uniform, logarithmic, settings.mSplitLogarithmicRatio) + settings.mSplitBias;
            const double minimumGap = std::max((settings.mDetailedDistance - settings.mNearDistance) * 1e-6,
                std::numeric_limits<double>::epsilon() * settings.mDetailedDistance * 16.0);
            const unsigned int remaining = settings.mDetailedCascadeCount - index;
            return std::clamp(requested, previous + minimumGap, settings.mDetailedDistance - minimumGap * remaining);
        }

        /// Add symmetric camera-depth overlap shared by each adjacent cascade pair.
        void applyTransitions(std::vector<ShadowCascade>& cascades, double transitionFraction)
        {
            for (std::size_t index = 0; index + 1 < cascades.size(); ++index)
            {
                ShadowCascade& inner = cascades[index];
                ShadowCascade& outer = cascades[index + 1];
                const double innerSpan = inner.mFarDistance - inner.mNearDistance;
                const double outerSpan = outer.mFarDistance - outer.mNearDistance;
                const double overlapWidth = transitionFraction * std::min(innerSpan, outerSpan);
                const double outwardOverlap = outer.mDistant ? 0.0 : 0.5 * overlapWidth;
                inner.mTransitionStartDistance = inner.mFarDistance - (overlapWidth - outwardOverlap);
                inner.mTransitionEndDistance = inner.mFarDistance + outwardOverlap;
                inner.mRenderFarDistance = inner.mTransitionEndDistance;
                outer.mRenderNearDistance = inner.mTransitionStartDistance;
            }
        }
    }

    std::vector<ShadowCascade> makeShadowCascadePlan(const ShadowCascadePlanSettings& settings)
    {
        validatePlanSettings(settings);
        const DistantCascadePolicy distantPolicy = distantCascadePolicy(settings.mDistantQuality);

        std::vector<double> boundaries;
        boundaries.reserve(settings.mMaximumCascadeCount + 1);
        boundaries.push_back(settings.mNearDistance);
        for (unsigned int index = 1; index < settings.mDetailedCascadeCount; ++index)
            boundaries.push_back(detailedSplit(settings, index, boundaries.back()));
        boundaries.push_back(settings.mDetailedDistance);

        if (settings.mExtendToVisibleDistance && settings.mVisibleDistance > settings.mDetailedDistance)
        {
            while (boundaries.size() <= settings.mMaximumCascadeCount && boundaries.back() < settings.mVisibleDistance)
            {
                const unsigned int remainingLevels
                    = settings.mMaximumCascadeCount - static_cast<unsigned int>(boundaries.size()) + 1;
                const double nextDistance = remainingLevels == 1
                    ? settings.mVisibleDistance
                    : std::min(settings.mVisibleDistance, boundaries.back() * distantPolicy.mDistanceMultiplier);
                boundaries.push_back(nextDistance);
            }
        }

        std::vector<ShadowCascade> cascades;
        cascades.reserve(boundaries.size() - 1);
        for (std::size_t index = 0; index + 1 < boundaries.size(); ++index)
        {
            const bool distant = index >= settings.mDetailedCascadeCount;
            const unsigned int distantIndex
                = distant ? static_cast<unsigned int>(index - settings.mDetailedCascadeCount) : 0;
            const unsigned int divisor = distant
                ? (distantIndex == 0 ? distantPolicy.mFirstResolutionDivisor : distantPolicy.mLaterResolutionDivisor)
                : 1u;
            const float minimumCasterPixels = distant ? std::min(std::ldexp(distantPolicy.mInitialMinimumCasterPixels,
                                                                     static_cast<int>(std::min(distantIndex, 4u))),
                                                  distantPolicy.mMaximumMinimumCasterPixels)
                                                      : 0.f;
            cascades.push_back({ boundaries[index], boundaries[index + 1], boundaries[index], boundaries[index + 1],
                boundaries[index + 1], boundaries[index + 1], divisor, minimumCasterPixels, distant });
        }
        applyTransitions(cascades, settings.mTransitionFraction);
        return cascades;
    }

    unsigned int makeShadowCasterTraversalMask(
        unsigned int configuredCasterMask, unsigned int distantWorldMask, bool distant)
    {
        return distant ? configuredCasterMask & distantWorldMask : configuredCasterMask;
    }

    StableShadowFootprint makeStableShadowFootprint(
        std::span<const osg::Vec3d> receiverPoints, double guardWorldUnits, double radiusQuantum)
    {
        if (receiverPoints.empty())
            throw std::invalid_argument("stable shadow footprint requires receiver points");
        if (!std::isfinite(guardWorldUnits) || guardWorldUnits < 0.0)
            throw std::invalid_argument("stable shadow footprint guard must be finite and nonnegative");
        if (!std::isfinite(radiusQuantum) || radiusQuantum <= 0.0)
            throw std::invalid_argument("stable shadow footprint quantum must be finite and positive");

        osg::Vec3d center;
        for (const osg::Vec3d& point : receiverPoints)
        {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || !std::isfinite(point.z()))
                throw std::invalid_argument("stable shadow footprint points must be finite");
            center += point;
        }
        center /= static_cast<double>(receiverPoints.size());

        double radius = 0.0;
        for (const osg::Vec3d& point : receiverPoints)
            radius = std::max(radius, (point - center).length());
        radius = std::ceil((radius + guardWorldUnits) / radiusQuantum) * radiusQuantum;
        return { center, radius };
    }

    StableDirectionalLightBasis makeStableDirectionalLightBasis(const osg::Vec3d& lightDirection)
    {
        if (!std::isfinite(lightDirection.x()) || !std::isfinite(lightDirection.y())
            || !std::isfinite(lightDirection.z()) || lightDirection.length2() <= 0.0)
            throw std::invalid_argument("stable shadow light direction must be finite and nonzero");

        osg::Vec3d direction = lightDirection;
        direction.normalize();

        // OpenMW's daylight points the incoming light direction into the lower world hemisphere. Constructing
        // the frame from its opposite keeps the only unavoidable spherical-basis singularity above the scene,
        // where exterior sunlight and its shadows are inactive.
        const double denominator = 1.0 - direction.z();
        if (denominator <= 1e-8)
            return { osg::Vec3d(1.0, 0.0, 0.0), osg::Vec3d(0.0, -1.0, 0.0) };

        const double reciprocal = 1.0 / denominator;
        osg::Vec3d side(1.0 - direction.x() * direction.x() * reciprocal, -direction.x() * direction.y() * reciprocal,
            direction.x());
        side.normalize();
        osg::Vec3d up = side ^ direction;
        up.normalize();
        return { side, up };
    }

    double makeNestedShadowTexelSize(
        double footprintRadius, unsigned int textureResolution, double minimumWorldTexelSize)
    {
        if (!std::isfinite(footprintRadius) || footprintRadius <= 0.0 || textureResolution == 0
            || !std::isfinite(minimumWorldTexelSize) || minimumWorldTexelSize <= 0.0)
            throw std::invalid_argument("nested shadow texel inputs must be finite and positive");
        const double requiredSize = 2.0 * footprintRadius / textureResolution;
        if (requiredSize <= minimumWorldTexelSize)
            return minimumWorldTexelSize;
        return minimumWorldTexelSize * std::pow(2.0, std::ceil(std::log2(requiredSize / minimumWorldTexelSize)));
    }

    ShadowCascadeSampling makeShadowCascadeSampling(
        double referenceWorldUnitsPerTexel, double cascadeWorldUnitsPerTexel)
    {
        if (!std::isfinite(referenceWorldUnitsPerTexel) || referenceWorldUnitsPerTexel <= 0.0
            || !std::isfinite(cascadeWorldUnitsPerTexel) || cascadeWorldUnitsPerTexel <= 0.0)
            throw std::invalid_argument("shadow cascade sampling scales require finite positive texel sizes");
        const float filterRadiusScale
            = static_cast<float>(std::clamp(referenceWorldUnitsPerTexel / cascadeWorldUnitsPerTexel, 0.35, 1.0));
        const float normalOffsetScale = static_cast<float>(std::clamp(
            cascadeWorldUnitsPerTexel / referenceWorldUnitsPerTexel * (1.0 + 0.25 * (1.0 - filterRadiusScale)), 1.0,
            8.0));
        return { filterRadiusScale, normalOffsetScale };
    }

    osg::Vec3d alignShadowFootprintToWorldTexels(const osg::Vec3d& viewRelativeLightSpaceCenter,
        const osg::Vec3d& worldOriginLightSpaceCenter, double footprintRadius, unsigned int textureResolution)
    {
        if (!std::isfinite(viewRelativeLightSpaceCenter.x()) || !std::isfinite(viewRelativeLightSpaceCenter.y())
            || !std::isfinite(viewRelativeLightSpaceCenter.z()) || !std::isfinite(worldOriginLightSpaceCenter.x())
            || !std::isfinite(worldOriginLightSpaceCenter.y()) || !std::isfinite(worldOriginLightSpaceCenter.z()))
            throw std::invalid_argument("shadow footprint centers must be finite");
        if (!std::isfinite(footprintRadius) || footprintRadius <= 0.0 || textureResolution == 0)
            throw std::invalid_argument("shadow footprint extent and resolution must be positive");
        const double worldUnitsPerTexel = 2.0 * footprintRadius / textureResolution;
        const double alignedWorldX
            = std::round(worldOriginLightSpaceCenter.x() / worldUnitsPerTexel) * worldUnitsPerTexel;
        const double alignedWorldY
            = std::round(worldOriginLightSpaceCenter.y() / worldUnitsPerTexel) * worldUnitsPerTexel;
        return { viewRelativeLightSpaceCenter.x() + alignedWorldX - worldOriginLightSpaceCenter.x(),
            viewRelativeLightSpaceCenter.y() + alignedWorldY - worldOriginLightSpaceCenter.y(),
            viewRelativeLightSpaceCenter.z() };
    }
}
