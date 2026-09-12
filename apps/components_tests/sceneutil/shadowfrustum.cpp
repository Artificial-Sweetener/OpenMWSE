#include <components/sceneutil/shadowfrustum.hpp>

#include <gtest/gtest.h>

#include <osg/Matrix>

#include <cmath>
#include <limits>

namespace
{
    /// Construct the deterministic camera volume shared by frustum geometry assertions.
    SceneUtil::ShadowFrustum makeTestFrustum(double farDistance = 1000.0)
    {
        osg::Matrixd projection;
        projection.makePerspective(60.0, 16.0 / 9.0, 1.0, farDistance);
        osg::Matrixd modelView;
        modelView.makeLookAt({ 100.0, 200.0, 30.0 }, { 100.0, 201.0, 30.0 }, { 0.0, 0.0, 1.0 });
        return { projection, modelView };
    }

    /// Preserve the original camera range while interpolating slice corners at requested camera depths.
    TEST(ShadowFrustumTest, InterpolatesStableCameraDepthSlices)
    {
        SceneUtil::ShadowFrustum frustum = makeTestFrustum();
        EXPECT_NEAR(frustum.mCameraNearDistance, 1.0, 1e-10);
        EXPECT_NEAR(frustum.mCameraFarDistance, 1000.0, 1e-8);
        frustum.initialize();
        const auto corners = frustum.sliceCorners(20.0, 400.0);
        for (std::size_t index = 0; index < 4; ++index)
        {
            EXPECT_NEAR((corners[index] - frustum.mEye) * frustum.mCenterLine, 20.0, 1e-8);
            EXPECT_NEAR((corners[index + 4] - frustum.mEye) * frustum.mCenterLine, 400.0, 1e-8);
        }
    }

    /// Reject invalid slice ranges before they can construct malformed cascade receiver volumes.
    TEST(ShadowFrustumTest, RejectsInvalidSliceRanges)
    {
        SceneUtil::ShadowFrustum frustum = makeTestFrustum();
        frustum.initialize();
        EXPECT_THROW(frustum.sliceCorners(10.0, 10.0), std::invalid_argument);
        EXPECT_THROW(frustum.sliceCorners(-1.0, 10.0), std::invalid_argument);
        EXPECT_THROW(frustum.sliceCorners(1.0, std::numeric_limits<double>::infinity()), std::invalid_argument);
    }

    /// Bound a receiver slice and every possible caster between it and a directional light.
    TEST(ShadowFrustumTest, BuildsDirectionalCasterVolumeForOneSlice)
    {
        SceneUtil::ShadowFrustum frustum = makeTestFrustum();
        frustum.initialize();
        const auto corners = frustum.sliceCorners(20.0, 400.0);
        osg::Vec3d lightDirection(-0.2, -0.3, -0.93);
        lightDirection.normalize();
        const osg::Polytope volume = SceneUtil::makeDirectionalShadowCullVolume(corners, lightDirection);
        ASSERT_FALSE(volume.empty());

        osg::Vec3d center;
        for (const osg::Vec3d& corner : corners)
        {
            center += corner;
            for (const osg::Plane& plane : volume.getPlaneList())
                EXPECT_GE(plane.distance(corner), -1e-8);
        }
        center /= static_cast<double>(corners.size());
        const osg::Vec3d casterTowardLight = center - lightDirection * 10000.0;
        for (const osg::Plane& plane : volume.getPlaneList())
            EXPECT_GE(plane.distance(casterTowardLight), -1e-8);
    }

    /// Keep a detailed caster volume independent of a farther camera horizon using the same view rays.
    TEST(ShadowFrustumTest, KeepsSliceCullVolumeIndependentOfCameraFarDistance)
    {
        SceneUtil::ShadowFrustum shortFrustum = makeTestFrustum(1000.0);
        SceneUtil::ShadowFrustum longFrustum = makeTestFrustum(10000.0);
        shortFrustum.initialize();
        longFrustum.initialize();
        const osg::Vec3d lightDirection(-0.2, -0.3, -0.93);
        const osg::Polytope shortVolume
            = SceneUtil::makeDirectionalShadowCullVolume(shortFrustum.sliceCorners(20.0, 400.0), lightDirection);
        const osg::Polytope longVolume
            = SceneUtil::makeDirectionalShadowCullVolume(longFrustum.sliceCorners(20.0, 400.0), lightDirection);
        ASSERT_EQ(shortVolume.getPlaneList().size(), longVolume.getPlaneList().size());
        for (std::size_t planeIndex = 0; planeIndex < shortVolume.getPlaneList().size(); ++planeIndex)
            for (unsigned int component = 0; component < 4; ++component)
                EXPECT_NEAR(shortVolume.getPlaneList()[planeIndex][component],
                    longVolume.getPlaneList()[planeIndex][component], 1e-10);
    }

    /// Reject malformed receiver geometry and light directions before constructing culling planes.
    TEST(ShadowFrustumTest, RejectsInvalidDirectionalCasterVolumes)
    {
        const std::array<osg::Vec3d, 7> tooFewCorners{};
        const std::array<osg::Vec3d, 8> corners{};
        EXPECT_THROW(
            SceneUtil::makeDirectionalShadowCullVolume(tooFewCorners, { 0.0, 0.0, -1.0 }), std::invalid_argument);
        EXPECT_THROW(SceneUtil::makeDirectionalShadowCullVolume(corners, {}), std::invalid_argument);
    }
}
