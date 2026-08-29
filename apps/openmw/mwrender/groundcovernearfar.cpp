#include "groundcovernearfar.hpp"

#include "renderworkcounter.hpp"

#include <algorithm>
#include <stdexcept>

#include <osg/Stats>
#include <osgUtil/CullVisitor>

#include <components/misc/convert.hpp>

namespace
{
    using Distance = osgUtil::CullVisitor::value_type;

    /// Calculate view-axis distance using the same equation as OSG's cull visitor.
    Distance viewDistance(const osg::Vec3& position, const osg::Matrix& matrix)
    {
        return -(static_cast<Distance>(position[0]) * static_cast<Distance>(matrix(0, 2))
            + static_cast<Distance>(position[1]) * static_cast<Distance>(matrix(1, 2))
            + static_cast<Distance>(position[2]) * static_cast<Distance>(matrix(2, 2)) + matrix(3, 2));
    }
}

namespace MWRender
{
    osg::Matrix makeGroundcoverInstanceMatrix(const osg::Vec4f& transform, const osg::Vec3f& rotation)
    {
        const float rotationValues[3]{ rotation.x(), rotation.y(), rotation.z() };
        return osg::Matrix::scale(transform.w(), transform.w(), transform.w())
            * osg::Matrix(Misc::Convert::makeOsgQuat(rotationValues))
            * osg::Matrix::translate(transform.x(), transform.y(), transform.z());
    }

    GroundcoverInstanceMatrices::GroundcoverInstanceMatrices(osg::Vec4Array* transforms, osg::Vec3Array* rotations)
        : mTransforms(transforms)
        , mRotations(rotations)
    {
        if (mTransforms == nullptr || mRotations == nullptr || mTransforms->size() != mRotations->size())
            throw std::invalid_argument("groundcover instance attributes must have matching sizes");
    }

    std::size_t GroundcoverInstanceMatrices::size() const noexcept
    {
        return mTransforms->size();
    }

    const std::vector<osg::Matrix>& GroundcoverInstanceMatrices::get() const
    {
        std::call_once(mOnce, [&] {
            mMatrices.reserve(mTransforms->size());
            for (std::size_t index = 0; index < mTransforms->size(); ++index)
                mMatrices.emplace_back(makeGroundcoverInstanceMatrix((*mTransforms)[index], (*mRotations)[index]));
        });
        return mMatrices;
    }

    GroundcoverNearFarCullCallback::GroundcoverNearFarCullCallback(
        osg::Vec4Array* transforms, osg::Vec3Array* rotations, const osg::BoundingBox& instanceBounds)
        : mInstanceMatrices(transforms, rotations)
        , mInstanceBounds(instanceBounds)
    {
    }

    bool GroundcoverNearFarCullCallback::cull(
        osg::NodeVisitor* visitor, osg::Drawable* drawable, osg::RenderInfo* renderInfo) const
    {
        osgUtil::CullVisitor& cullVisitor = *visitor->asCullVisitor();
        const osg::CullSettings::ComputeNearFarMode nearFarMode = cullVisitor.getComputeNearFarMode();
        const osg::BoundingBox& boundingBox = drawable->getBoundingBox();
        osg::RefMatrix& matrix = *cullVisitor.getModelViewMatrix();

        osg::Stats* stats = cullVisitor.getCurrentCamera()->getStats();
        if (stats)
            addDiagnosticRenderWorkCounter(*stats, cullVisitor.getTraversalNumber(), groundcoverInstanceRenderCounter,
                static_cast<double>(mInstanceMatrices.size()));

        if (nearFarMode != osg::CullSettings::COMPUTE_NEAR_FAR_USING_PRIMITIVES
            && nearFarMode != osg::CullSettings::COMPUTE_NEAR_USING_PRIMITIVES)
            return false;

        double testedInstances = 0.0;
        if (drawable->isCullingActive() && cullVisitor.isCulled(boundingBox))
            return true;

        const osg::Vec3 lookVector = cullVisitor.getLookVectorLocal();
        const unsigned int boundingBoxCornerFar
            = (lookVector.x() >= 0 ? 1 : 0) | (lookVector.y() >= 0 ? 2 : 0) | (lookVector.z() >= 0 ? 4 : 0);
        const unsigned int boundingBoxCornerNear = (~boundingBoxCornerFar) & 7;
        Distance nearDistance = viewDistance(boundingBox.corner(boundingBoxCornerNear), matrix);
        Distance farDistance = viewDistance(boundingBox.corner(boundingBoxCornerFar), matrix);

        if (nearDistance > farDistance)
            std::swap(nearDistance, farDistance);

        if (farDistance < 0)
            return true;

        Distance computedNear = cullVisitor.getCalculatedNearPlane();
        Distance computedFar = cullVisitor.getCalculatedFarPlane();
        const std::vector<osg::Matrix>& instanceMatrices = mInstanceMatrices.get();

        if (nearDistance < computedNear || farDistance > computedFar)
        {
            osg::Polytope frustum;
            const osg::Polytope::ClippingMask resultMask
                = cullVisitor.getCurrentCullingSet().getFrustum().getResultMask();
            if (resultMask)
            {
                cullVisitor.computeNearPlane();
                computedNear = cullVisitor.getCalculatedNearPlane();
                computedFar = cullVisitor.getCalculatedFarPlane();

                if (nearDistance < computedNear)
                {
                    nearDistance = computedNear;
                    for (const osg::Matrix& instanceMatrix : instanceMatrices)
                    {
                        testedInstances += 1.0;
                        const osg::Matrix fullMatrix = instanceMatrix * matrix;
                        const osg::Vec3d instanceLookVector(
                            -fullMatrix(0, 2), -fullMatrix(1, 2), -fullMatrix(2, 2));
                        const unsigned int instanceCornerFar = (instanceLookVector.x() >= 0 ? 1 : 0)
                            | (instanceLookVector.y() >= 0 ? 2 : 0) | (instanceLookVector.z() >= 0 ? 4 : 0);
                        const unsigned int instanceCornerNear = (~instanceCornerFar) & 7;
                        Distance instanceNear = viewDistance(mInstanceBounds.corner(instanceCornerNear), fullMatrix);
                        Distance instanceFar = viewDistance(mInstanceBounds.corner(instanceCornerFar), fullMatrix);

                        if (instanceNear > instanceFar)
                            std::swap(instanceNear, instanceFar);
                        if (instanceFar < 0 || instanceNear > nearDistance)
                            continue;

                        frustum.setAndTransformProvidingInverse(
                            cullVisitor.getProjectionCullingStack().back().getFrustum(), fullMatrix);
                        osg::Polytope::PlaneList planes;
                        osg::Polytope::ClippingMask selectorMask = 0x1;
                        for (const osg::Plane& plane : frustum.getPlaneList())
                        {
                            if (resultMask & selectorMask)
                                planes.push_back(plane);
                            selectorMask <<= 1;
                        }

                        const Distance candidate
                            = cullVisitor.computeNearestPointInFrustum(fullMatrix, planes, *drawable);
                        nearDistance = std::min(nearDistance, candidate);
                    }
                    if (nearDistance < computedNear)
                        cullVisitor.setCalculatedNearPlane(nearDistance);
                }

                if (nearFarMode == osg::CullSettings::COMPUTE_NEAR_FAR_USING_PRIMITIVES && farDistance > computedFar)
                {
                    farDistance = computedFar;
                    for (const osg::Matrix& instanceMatrix : instanceMatrices)
                    {
                        testedInstances += 1.0;
                        const osg::Matrix fullMatrix = instanceMatrix * matrix;
                        const osg::Vec3d instanceLookVector(
                            -fullMatrix(0, 2), -fullMatrix(1, 2), -fullMatrix(2, 2));
                        const unsigned int instanceCornerFar = (instanceLookVector.x() >= 0 ? 1 : 0)
                            | (instanceLookVector.y() >= 0 ? 2 : 0) | (instanceLookVector.z() >= 0 ? 4 : 0);
                        const unsigned int instanceCornerNear = (~instanceCornerFar) & 7;
                        Distance instanceNear = viewDistance(mInstanceBounds.corner(instanceCornerNear), fullMatrix);
                        Distance instanceFar = viewDistance(mInstanceBounds.corner(instanceCornerFar), fullMatrix);

                        if (instanceNear > instanceFar)
                            std::swap(instanceNear, instanceFar);
                        if (instanceFar < 0 || instanceFar < farDistance)
                            continue;

                        frustum.setAndTransformProvidingInverse(
                            cullVisitor.getProjectionCullingStack().back().getFrustum(), fullMatrix);
                        osg::Polytope::PlaneList planes;
                        osg::Polytope::ClippingMask selectorMask = 0x1;
                        for (const osg::Plane& plane : frustum.getPlaneList())
                        {
                            if (resultMask & selectorMask)
                                planes.push_back(plane);
                            selectorMask <<= 1;
                        }

                        const Distance candidate
                            = cullVisitor.computeFurthestPointInFrustum(fullMatrix, planes, *drawable);
                        farDistance = std::max(farDistance, candidate);
                    }
                    if (farDistance > computedFar)
                        cullVisitor.setCalculatedFarPlane(farDistance);
                }
            }
        }

        if (stats)
            addDiagnosticRenderWorkCounter(
                *stats, cullVisitor.getTraversalNumber(), groundcoverInstanceTestCounter, testedInstances);
        return false;
    }
}
