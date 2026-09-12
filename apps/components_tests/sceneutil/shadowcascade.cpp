#include <components/sceneutil/shadowcascade.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace
{
    /// Retain practical near detail while producing monotonic bounded cascade intervals.
    TEST(ShadowCascade, BuildsStableDetailedDistribution)
    {
        const auto cascades = SceneUtil::makeShadowCascadePlan({ .mNearDistance = 1.0,
            .mDetailedDistance = 8192.0,
            .mVisibleDistance = 8192.0,
            .mSplitLogarithmicRatio = 0.5,
            .mSplitBias = 0.0,
            .mTransitionFraction = 0.08,
            .mDetailedCascadeCount = 3,
            .mMaximumCascadeCount = 8,
            .mExtendToVisibleDistance = false });
        ASSERT_EQ(cascades.size(), 3);
        EXPECT_DOUBLE_EQ(cascades.front().mNearDistance, 1.0);
        EXPECT_DOUBLE_EQ(cascades.back().mFarDistance, 8192.0);
        for (std::size_t index = 0; index < cascades.size(); ++index)
        {
            EXPECT_LT(cascades[index].mNearDistance, cascades[index].mFarDistance);
            EXPECT_FALSE(cascades[index].mDistant);
            EXPECT_EQ(cascades[index].mResolutionDivisor, 1u);
            EXPECT_FLOAT_EQ(cascades[index].mMinimumCasterPixels, 0.f);
            if (index > 0)
                EXPECT_DOUBLE_EQ(cascades[index - 1].mFarDistance, cascades[index].mNearDistance);
        }
    }

    /// Cover the visible horizon with geometrically expanding, progressively cheaper far levels.
    TEST(ShadowCascade, ExtendsDetailedCoverageToVisibleDistance)
    {
        const auto cascades = SceneUtil::makeShadowCascadePlan({ .mNearDistance = 1.0,
            .mDetailedDistance = 8192.0,
            .mVisibleDistance = 204800.0,
            .mSplitLogarithmicRatio = 0.5,
            .mSplitBias = 0.0,
            .mTransitionFraction = 0.08,
            .mDetailedCascadeCount = 3,
            .mMaximumCascadeCount = 8,
            .mExtendToVisibleDistance = true,
            .mDistantQuality = SceneUtil::DistantShadowQuality::Low });
        ASSERT_EQ(cascades.size(), 6);
        EXPECT_DOUBLE_EQ(cascades[2].mFarDistance, 8192.0);
        EXPECT_DOUBLE_EQ(cascades[3].mFarDistance, 32768.0);
        EXPECT_DOUBLE_EQ(cascades[4].mFarDistance, 131072.0);
        EXPECT_DOUBLE_EQ(cascades[5].mFarDistance, 204800.0);
        EXPECT_EQ(cascades[3].mResolutionDivisor, 4u);
        EXPECT_EQ(cascades[4].mResolutionDivisor, 8u);
        EXPECT_EQ(cascades[5].mResolutionDivisor, 8u);
        EXPECT_FLOAT_EQ(cascades[3].mMinimumCasterPixels, 4.f);
        EXPECT_FLOAT_EQ(cascades[4].mMinimumCasterPixels, 8.f);
        EXPECT_FLOAT_EQ(cascades[5].mMinimumCasterPixels, 16.f);
        EXPECT_TRUE(cascades[5].mDistant);
    }

    /// Increase horizon density and caster retention monotonically without changing configured nearby coverage.
    TEST(ShadowCascade, AppliesDistantQualityTiers)
    {
        struct ExpectedTier
        {
            SceneUtil::DistantShadowQuality mQuality;
            std::size_t mCascadeCount;
            unsigned int mFirstDivisor;
            unsigned int mLaterDivisor;
            float mFirstCasterPixels;
        };
        constexpr std::array tiers{
            ExpectedTier{ SceneUtil::DistantShadowQuality::Low, 5, 4, 8, 4.f },
            ExpectedTier{ SceneUtil::DistantShadowQuality::Medium, 7, 2, 4, 2.f },
            ExpectedTier{ SceneUtil::DistantShadowQuality::High, 7, 2, 2, 1.f },
            ExpectedTier{ SceneUtil::DistantShadowQuality::Ultra, 7, 1, 2, 0.5f },
        };
        for (const ExpectedTier& tier : tiers)
        {
            const auto cascades = SceneUtil::makeShadowCascadePlan({ .mNearDistance = 1.0,
                .mDetailedDistance = 24576.0,
                .mVisibleDistance = 204800.0,
                .mSplitLogarithmicRatio = 0.5,
                .mSplitBias = 0.0,
                .mTransitionFraction = 0.08,
                .mDetailedCascadeCount = 3,
                .mMaximumCascadeCount = 8,
                .mExtendToVisibleDistance = true,
                .mDistantQuality = tier.mQuality });
            ASSERT_EQ(cascades.size(), tier.mCascadeCount);
            EXPECT_DOUBLE_EQ(cascades[2].mFarDistance, 24576.0);
            EXPECT_DOUBLE_EQ(cascades.back().mFarDistance, 204800.0);
            EXPECT_EQ(cascades[3].mResolutionDivisor, tier.mFirstDivisor);
            EXPECT_EQ(cascades[4].mResolutionDivisor, tier.mLaterDivisor);
            EXPECT_FLOAT_EQ(cascades[3].mMinimumCasterPixels, tier.mFirstCasterPixels);
        }
    }

    /// Retain the configured caster categories nearby and world geometry alone in distant levels.
    TEST(ShadowCascade, SelectsDistantWorldCasterMask)
    {
        constexpr unsigned int sceneRootMask = 0b000001u;
        constexpr unsigned int actorMask = 0b000010u;
        constexpr unsigned int worldMask = 0b011100u;
        constexpr unsigned int configuredMask = sceneRootMask | actorMask | worldMask;
        constexpr unsigned int distantWorldTraversalMask = sceneRootMask | worldMask;
        EXPECT_EQ(SceneUtil::makeShadowCasterTraversalMask(configuredMask, worldMask, false), configuredMask);
        EXPECT_EQ(SceneUtil::makeShadowCasterTraversalMask(configuredMask, distantWorldTraversalMask, true),
            distantWorldTraversalMask);
    }

    /// Reach the visible horizon even when hardware leaves room for only one distant level.
    TEST(ShadowCascade, FitsHorizonCoverageWithinAvailableMapCount)
    {
        const auto cascades = SceneUtil::makeShadowCascadePlan({ .mNearDistance = 1.0,
            .mDetailedDistance = 8192.0,
            .mVisibleDistance = 204800.0,
            .mSplitLogarithmicRatio = 0.5,
            .mSplitBias = 0.0,
            .mTransitionFraction = 0.08,
            .mDetailedCascadeCount = 3,
            .mMaximumCascadeCount = 4,
            .mExtendToVisibleDistance = true });
        ASSERT_EQ(cascades.size(), 4);
        EXPECT_DOUBLE_EQ(cascades.back().mNearDistance, 8192.0);
        EXPECT_DOUBLE_EQ(cascades.back().mFarDistance, 204800.0);
        EXPECT_TRUE(cascades.back().mDistant);
    }

    /// Give adjacent maps identical depth transition bands without creating coverage gaps.
    TEST(ShadowCascade, BuildsSymmetricTransitionCoverage)
    {
        const auto cascades = SceneUtil::makeShadowCascadePlan({ .mNearDistance = 1.0,
            .mDetailedDistance = 1000.0,
            .mVisibleDistance = 1000.0,
            .mSplitLogarithmicRatio = 0.0,
            .mSplitBias = 0.0,
            .mTransitionFraction = 0.1,
            .mDetailedCascadeCount = 2,
            .mMaximumCascadeCount = 2,
            .mExtendToVisibleDistance = false });
        ASSERT_EQ(cascades.size(), 2);
        EXPECT_DOUBLE_EQ(cascades[0].mTransitionStartDistance, cascades[1].mRenderNearDistance);
        EXPECT_DOUBLE_EQ(cascades[0].mTransitionEndDistance, cascades[0].mRenderFarDistance);
        EXPECT_LT(cascades[0].mTransitionStartDistance, cascades[0].mFarDistance);
        EXPECT_GT(cascades[0].mTransitionEndDistance, cascades[1].mNearDistance);
    }

    /// Blend into distant levels using inward overlap without expanding a full-resolution detailed cascade.
    TEST(ShadowCascade, KeepsDistantTransitionInsideDetailedCoverage)
    {
        const auto cascades = SceneUtil::makeShadowCascadePlan({ .mNearDistance = 1.0,
            .mDetailedDistance = 1000.0,
            .mVisibleDistance = 2000.0,
            .mSplitLogarithmicRatio = 0.0,
            .mSplitBias = 0.0,
            .mTransitionFraction = 0.1,
            .mDetailedCascadeCount = 2,
            .mMaximumCascadeCount = 3,
            .mExtendToVisibleDistance = true });
        ASSERT_EQ(cascades.size(), 3);
        EXPECT_DOUBLE_EQ(cascades[1].mRenderFarDistance, cascades[1].mFarDistance);
        EXPECT_DOUBLE_EQ(cascades[1].mTransitionEndDistance, cascades[1].mFarDistance);
        EXPECT_LT(cascades[2].mRenderNearDistance, cascades[2].mNearDistance);
        EXPECT_DOUBLE_EQ(cascades[1].mTransitionStartDistance, cascades[2].mRenderNearDistance);
    }

    /// Keep the fitted footprint radius invariant under rigid camera rotation and translation.
    TEST(ShadowCascade, StabilizesReceiverFootprintScale)
    {
        const std::array<osg::Vec3d, 8> points{ osg::Vec3d(-2, -1, 2), osg::Vec3d(2, -1, 2), osg::Vec3d(-8, -4, 10),
            osg::Vec3d(8, -4, 10), osg::Vec3d(-2, 1, 2), osg::Vec3d(2, 1, 2), osg::Vec3d(-8, 4, 10),
            osg::Vec3d(8, 4, 10) };
        std::array<osg::Vec3d, 8> transformed;
        constexpr double angle = 0.73;
        for (std::size_t index = 0; index < points.size(); ++index)
            transformed[index]
                = osg::Vec3d(points[index].x() * std::cos(angle) - points[index].y() * std::sin(angle),
                      points[index].x() * std::sin(angle) + points[index].y() * std::cos(angle), points[index].z())
                + osg::Vec3d(1700, -920, 44);
        const auto original = SceneUtil::makeStableShadowFootprint(points, 2.0, 1.0 / 16.0);
        const auto moved = SceneUtil::makeStableShadowFootprint(transformed, 2.0, 1.0 / 16.0);
        EXPECT_DOUBLE_EQ(original.mRadius, moved.mRadius);
        EXPECT_NEAR((moved.mCenter - osg::Vec3d(1700, -920, 44)).length(), original.mCenter.length(), 1e-10);
    }

    /// Keep the shadow-map axes orthonormal and independent of camera heading for representative daylight directions.
    TEST(ShadowCascade, BuildsWorldAnchoredDirectionalLightBasis)
    {
        for (const osg::Vec3d& direction : { osg::Vec3d(-0.16, -0.21, -0.96), osg::Vec3d(0.96, -0.18, -0.22),
                 osg::Vec3d(-0.96, -0.18, -0.22), osg::Vec3d(0.0, 0.6, -0.8) })
        {
            const auto basis = SceneUtil::makeStableDirectionalLightBasis(direction);
            osg::Vec3d normalizedDirection = direction;
            normalizedDirection.normalize();
            EXPECT_NEAR(basis.mSide.length(), 1.0, 1e-12);
            EXPECT_NEAR(basis.mUp.length(), 1.0, 1e-12);
            EXPECT_NEAR(basis.mSide * basis.mUp, 0.0, 1e-12);
            EXPECT_NEAR(basis.mSide * normalizedDirection, 0.0, 1e-12);
            EXPECT_NEAR(basis.mUp * normalizedDirection, 0.0, 1e-12);
        }
    }

    /// Change shadow-map roll continuously as the sun moves instead of switching between camera-derived axes.
    TEST(ShadowCascade, MovesDirectionalLightBasisContinuously)
    {
        osg::Vec3d before(-0.1608, -0.2109, -0.9641);
        osg::Vec3d after(-0.1607, -0.2109, -0.9642);
        const auto beforeBasis = SceneUtil::makeStableDirectionalLightBasis(before);
        const auto afterBasis = SceneUtil::makeStableDirectionalLightBasis(after);
        EXPECT_GT(beforeBasis.mSide * afterBasis.mSide, 0.999999);
        EXPECT_GT(beforeBasis.mUp * afterBasis.mUp, 0.999999);
    }

    /// Cancel sub-texel camera travel so the projected footprint remains on a world-origin light-space grid.
    TEST(ShadowCascade, AlignsFootprintCenterToWorldTexels)
    {
        const osg::Vec3d initialViewOrigin(8.2, -11.2, 30.0);
        const osg::Vec3d initialWorldCenter(13.2, -7.2, 41.25);
        const osg::Vec3d initialViewRelativeCenter = initialWorldCenter - initialViewOrigin;
        const osg::Vec3d initialAligned
            = SceneUtil::alignShadowFootprintToWorldTexels(initialViewRelativeCenter, initialWorldCenter, 64.0, 128);
        EXPECT_DOUBLE_EQ((initialAligned + initialViewOrigin).x(), 13.0);
        EXPECT_DOUBLE_EQ((initialAligned + initialViewOrigin).y(), -7.0);
        EXPECT_DOUBLE_EQ(initialAligned.z(), initialViewRelativeCenter.z());

        const osg::Vec3d movedViewOrigin(8.4, -11.4, 30.0);
        const osg::Vec3d movedWorldCenter(13.4, -7.4, 41.25);
        const osg::Vec3d movedViewRelativeCenter = movedWorldCenter - movedViewOrigin;
        const osg::Vec3d movedAligned
            = SceneUtil::alignShadowFootprintToWorldTexels(movedViewRelativeCenter, movedWorldCenter, 64.0, 128);
        EXPECT_DOUBLE_EQ((movedAligned + movedViewOrigin).x(), 13.0);
        EXPECT_DOUBLE_EQ((movedAligned + movedViewOrigin).y(), -7.0);
    }

    /// Keep each coarser grid an integer power-of-two multiple of the finest world grid.
    TEST(ShadowCascade, BuildsNestedWorldTexelSizes)
    {
        const double nearTexel = SceneUtil::makeNestedShadowTexelSize(750.0, 2048, 1.0 / 64.0);
        const double middleTexel = SceneUtil::makeNestedShadowTexelSize(2800.0, 2048, 1.0 / 64.0);
        const double distantTexel = SceneUtil::makeNestedShadowTexelSize(12000.0, 1024, 1.0 / 64.0);
        EXPECT_DOUBLE_EQ(std::fmod(middleTexel, nearTexel), 0.0);
        EXPECT_DOUBLE_EQ(std::fmod(distantTexel, middleTexel), 0.0);
        EXPECT_GE(nearTexel, 1500.0 / 2048.0);
        EXPECT_GE(middleTexel, 5600.0 / 2048.0);
        EXPECT_GE(distantTexel, 24000.0 / 1024.0);
    }

    /// Narrow coarse-map kernels while increasing their bounded texel-aware bias compensation.
    TEST(ShadowCascade, AdaptsSamplingToWorldTexelSize)
    {
        const auto detailed = SceneUtil::makeShadowCascadeSampling(1.0, 1.0);
        EXPECT_FLOAT_EQ(detailed.mFilterRadiusScale, 1.f);
        EXPECT_FLOAT_EQ(detailed.mNormalOffsetScale, 1.f);
        const auto distant = SceneUtil::makeShadowCascadeSampling(1.0, 8.0);
        EXPECT_FLOAT_EQ(distant.mFilterRadiusScale, 0.35f);
        EXPECT_FLOAT_EQ(distant.mNormalOffsetScale, 8.f);
    }

    /// Reject non-finite and structurally invalid policy before render-thread use.
    TEST(ShadowCascade, RejectsInvalidInputs)
    {
        SceneUtil::ShadowCascadePlanSettings invalid;
        invalid.mDetailedCascadeCount = 0;
        EXPECT_THROW(SceneUtil::makeShadowCascadePlan(invalid), std::invalid_argument);
        invalid = {};
        invalid.mVisibleDistance = std::numeric_limits<double>::infinity();
        EXPECT_THROW(SceneUtil::makeShadowCascadePlan(invalid), std::invalid_argument);
        const std::array points{ osg::Vec3d() };
        EXPECT_THROW(SceneUtil::makeStableShadowFootprint(points, -1.0, 0.25), std::invalid_argument);
        EXPECT_THROW(SceneUtil::makeStableDirectionalLightBasis({}), std::invalid_argument);
        EXPECT_THROW(SceneUtil::makeStableDirectionalLightBasis({ std::numeric_limits<double>::infinity(), 0.0, -1.0 }),
            std::invalid_argument);
        EXPECT_THROW(SceneUtil::makeNestedShadowTexelSize(1.0, 0, 1.0 / 64.0), std::invalid_argument);
        EXPECT_THROW(SceneUtil::makeShadowCascadeSampling(1.0, 0.0), std::invalid_argument);
        EXPECT_THROW(SceneUtil::alignShadowFootprintToWorldTexels({}, {}, 1.0, 0), std::invalid_argument);
    }
}
