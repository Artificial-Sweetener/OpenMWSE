#ifndef OPENMW_COMPONENTS_SCENEUTIL_SHADOWFRUSTUM_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_SHADOWFRUSTUM_HPP

#include <array>
#include <span>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/Polytope>
#include <osg/Vec3d>

namespace osgUtil
{
    class CullVisitor;
}

namespace SceneUtil
{
    /// Own the camera-frustum geometry shared by directional shadow planning and culling.
    class ShadowFrustum
    {
    public:
        using Vertices = std::vector<osg::Vec3d>;
        using Indices = std::vector<unsigned int>;
        using Faces = std::vector<Indices>;
        using Edges = std::vector<Indices>;

        /// Capture an unbounded camera projection for deterministic frustum geometry and tests.
        ShadowFrustum(const osg::Matrixd& projectionMatrix, const osg::Matrixd& modelViewMatrix);

        /// Capture the camera projection and clamp only the working copy to visible scene bounds.
        ShadowFrustum(osgUtil::CullVisitor& visitor, double minimumNearDistance, double maximumFarDistance);

        /// Replace the canonical clip cube for stereo or another explicitly shared camera volume.
        void setCustomClipSpace(const osg::BoundingBoxd& customClipSpace);

        /// Transform clip corners to world space and establish indexed faces and edges.
        void initialize();

        /// Interpolate the four frustum edge rays at two camera-depth planes.
        std::array<osg::Vec3d, 8> sliceCorners(double nearDistance, double farDistance) const;

        osg::Matrixd mProjectionMatrix;
        osg::Matrixd mModelViewMatrix;
        bool mUseCustomClipSpace = false;
        osg::BoundingBoxd mCustomClipSpace;
        Vertices mCorners;
        Faces mFaces;
        Edges mEdges;
        osg::Vec3d mEye;
        osg::Vec3d mCenterNearPlane;
        osg::Vec3d mCenterFarPlane;
        osg::Vec3d mCenter;
        osg::Vec3d mCenterLine;
        double mCameraNearDistance = 0.0;
        double mCameraFarDistance = 0.0;
    };

    /// Extrude one receiver slice toward a directional light to bound only casters that can affect it.
    osg::Polytope makeDirectionalShadowCullVolume(
        std::span<const osg::Vec3d> receiverCorners, const osg::Vec3d& lightDirection);
}

#endif
