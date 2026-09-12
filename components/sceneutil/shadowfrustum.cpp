#include "shadowfrustum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include <osg/Notify>
#include <osg/io_utils>
#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    ShadowFrustum::ShadowFrustum(const osg::Matrixd& projectionMatrix, const osg::Matrixd& modelViewMatrix)
        : mProjectionMatrix(projectionMatrix)
        , mModelViewMatrix(modelViewMatrix)
        , mCorners(8)
        , mFaces(6)
        , mEdges(12)
    {
        double left = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        double top = 0.0;
        if (!mProjectionMatrix.getFrustum(left, right, bottom, top, mCameraNearDistance, mCameraFarDistance))
            mProjectionMatrix.getOrtho(left, right, bottom, top, mCameraNearDistance, mCameraFarDistance);
    }

    ShadowFrustum::ShadowFrustum(osgUtil::CullVisitor& visitor, double minimumNearDistance, double maximumFarDistance)
        : ShadowFrustum(*visitor.getProjectionMatrix(), *visitor.getModelViewMatrix())
    {
        OSG_INFO << "Projection matrix " << mProjectionMatrix << std::endl;
        if (visitor.getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR)
        {
            auto nearDistance = std::max(visitor.getCalculatedNearPlane(), minimumNearDistance);
            auto farDistance = std::min(visitor.getCalculatedFarPlane(), maximumFarDistance);
            if (farDistance < 0.0)
                farDistance = minimumNearDistance;
            nearDistance = std::min(nearDistance, farDistance);
            visitor.clampProjectionMatrix(mProjectionMatrix, nearDistance, farDistance);
            OSG_INFO << "zNear = " << nearDistance << ", zFar = " << farDistance << std::endl;
            OSG_INFO << "Projection matrix after clamping " << mProjectionMatrix << std::endl;
        }
    }

    void ShadowFrustum::setCustomClipSpace(const osg::BoundingBoxd& customClipSpace)
    {
        mUseCustomClipSpace = true;
        mCustomClipSpace = customClipSpace;
    }

    void ShadowFrustum::initialize()
    {
        osg::Matrixd clipToWorld;
        clipToWorld.invert(mModelViewMatrix * mProjectionMatrix);
        if (mUseCustomClipSpace)
        {
            mCorners.clear();
            for (const int index : { 0, 1, 5, 4, 2, 3, 7, 6 })
                mCorners.push_back(mCustomClipSpace.corner(index));
        }
        else
        {
            mCorners[0].set(-1.0, -1.0, -1.0);
            mCorners[1].set(1.0, -1.0, -1.0);
            mCorners[2].set(1.0, -1.0, 1.0);
            mCorners[3].set(-1.0, -1.0, 1.0);
            mCorners[4].set(-1.0, 1.0, -1.0);
            mCorners[5].set(1.0, 1.0, -1.0);
            mCorners[6].set(1.0, 1.0, 1.0);
            mCorners[7].set(-1.0, 1.0, 1.0);
        }
        for (osg::Vec3d& corner : mCorners)
        {
            corner = corner * clipToWorld;
            OSG_INFO << "   corner " << corner << std::endl;
        }

        mEye = osg::Vec3d() * osg::Matrix::inverse(mModelViewMatrix);
        mCenterNearPlane = (mCorners[0] + mCorners[1] + mCorners[5] + mCorners[4]) * 0.25;
        mCenterFarPlane = (mCorners[3] + mCorners[2] + mCorners[6] + mCorners[7]) * 0.25;
        mCenter = (mCenterNearPlane + mCenterFarPlane) * 0.5;
        mCenterLine = mCenterFarPlane - mCenterNearPlane;
        mCenterLine.normalize();
        OSG_INFO << "   center " << mCenter << std::endl;

        mFaces = { { 0, 3, 7, 4 }, { 1, 5, 6, 2 }, { 0, 1, 2, 3 }, { 4, 7, 6, 5 }, { 0, 4, 5, 1 }, { 2, 6, 7, 3 } };
        mEdges = { { 0, 1, 2, 4 }, { 1, 2, 2, 1 }, { 2, 3, 2, 5 }, { 3, 0, 2, 0 }, { 0, 4, 0, 4 }, { 1, 5, 4, 1 },
            { 2, 6, 1, 5 }, { 3, 7, 5, 0 }, { 4, 5, 3, 4 }, { 5, 6, 3, 1 }, { 6, 7, 3, 5 }, { 7, 4, 3, 0 } };
    }

    std::array<osg::Vec3d, 8> ShadowFrustum::sliceCorners(double nearDistance, double farDistance) const
    {
        if (!std::isfinite(nearDistance) || !std::isfinite(farDistance) || nearDistance < 0.0
            || farDistance <= nearDistance || mCorners.size() != 8)
            throw std::invalid_argument("shadow frustum slice distances and geometry must be valid");
        const double frustumNearDistance = (mCenterNearPlane - mEye) * mCenterLine;
        const double frustumFarDistance = (mCenterFarPlane - mEye) * mCenterLine;
        if (!std::isfinite(frustumNearDistance) || !std::isfinite(frustumFarDistance)
            || frustumFarDistance <= frustumNearDistance)
            throw std::invalid_argument("shadow frustum depth interval must be finite and ordered");

        constexpr std::array<unsigned int, 4> nearIndices{ 0, 1, 5, 4 };
        constexpr std::array<unsigned int, 4> farIndices{ 3, 2, 6, 7 };
        const auto interpolate = [&](unsigned int index, double distance) {
            const double factor = (distance - frustumNearDistance) / (frustumFarDistance - frustumNearDistance);
            return mCorners[nearIndices[index]] + (mCorners[farIndices[index]] - mCorners[nearIndices[index]]) * factor;
        };
        std::array<osg::Vec3d, 8> result;
        for (unsigned int index = 0; index < 4; ++index)
        {
            result[index] = interpolate(index, nearDistance);
            result[index + 4] = interpolate(index, farDistance);
        }
        return result;
    }

    osg::Polytope makeDirectionalShadowCullVolume(
        std::span<const osg::Vec3d> receiverCorners, const osg::Vec3d& lightDirection)
    {
        if (receiverCorners.size() != 8 || !std::isfinite(lightDirection.x()) || !std::isfinite(lightDirection.y())
            || !std::isfinite(lightDirection.z()) || lightDirection.length2() <= 0.0)
            throw std::invalid_argument("directional shadow cull volume inputs must be valid");

        osg::Vec3d center;
        for (const osg::Vec3d& corner : receiverCorners)
        {
            if (!std::isfinite(corner.x()) || !std::isfinite(corner.y()) || !std::isfinite(corner.z()))
                throw std::invalid_argument("directional shadow receiver corners must be finite");
            center += corner;
        }
        center /= static_cast<double>(receiverCorners.size());

        constexpr std::array<std::array<unsigned int, 4>, 6> faces{ { { 0, 3, 7, 4 }, { 1, 5, 6, 2 }, { 0, 1, 2, 3 },
            { 4, 7, 6, 5 }, { 0, 4, 5, 1 }, { 2, 6, 7, 3 } } };
        constexpr std::array<std::array<unsigned int, 4>, 12> edges{ { { 0, 1, 2, 4 }, { 1, 2, 2, 1 }, { 2, 3, 2, 5 },
            { 3, 0, 2, 0 }, { 0, 4, 0, 4 }, { 1, 5, 4, 1 }, { 2, 6, 1, 5 }, { 3, 7, 5, 0 }, { 4, 5, 3, 4 },
            { 5, 6, 3, 1 }, { 6, 7, 3, 5 }, { 7, 4, 3, 0 } } };

        osg::Vec3d normalizedLightDirection = lightDirection;
        normalizedLightDirection.normalize();
        std::array<bool, faces.size()> activeFaces{};
        osg::Polytope result;
        for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
        {
            const auto& face = faces[faceIndex];
            osg::Plane plane(receiverCorners[face[0]], receiverCorners[face[1]], receiverCorners[face[2]]);
            if (plane.distance(center) < 0.0)
                plane.flip();
            activeFaces[faceIndex] = plane.dotProductNormal(normalizedLightDirection) < 0.0;
            if (activeFaces[faceIndex])
                result.add(plane);
        }

        for (const auto& edge : edges)
        {
            if (activeFaces[edge[2]] == activeFaces[edge[3]])
                continue;
            osg::Vec3d normal = (receiverCorners[edge[1]] - receiverCorners[edge[0]]) ^ normalizedLightDirection;
            if (normal.length2() <= 0.0)
                continue;
            normal.normalize();
            osg::Plane boundary(normal, receiverCorners[edge[0]]);
            if (boundary.distance(center) < 0.0)
                boundary.flip();
            result.add(boundary);
        }
        result.setupMask();
        return result;
    }
}
