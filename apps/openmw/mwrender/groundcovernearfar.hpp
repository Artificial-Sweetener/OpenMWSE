#ifndef OPENMW_MWRENDER_GROUNDCOVERNEARFAR_H
#define OPENMW_MWRENDER_GROUNDCOVERNEARFAR_H

#include <mutex>
#include <vector>

#include <osg/Array>
#include <osg/BoundingBox>
#include <osg/Drawable>
#include <osg/Matrix>
#include <osg/ref_ptr>

namespace MWRender
{
    /// Reconstruct one groundcover instance matrix from the compact attributes supplied to the GPU.
    osg::Matrix makeGroundcoverInstanceMatrix(const osg::Vec4f& transform, const osg::Vec3f& rotation);

    /// Materialize exact groundcover instance matrices once when a camera needs primitive near/far bounds.
    class GroundcoverInstanceMatrices
    {
    public:
        /// Retain the immutable compact attributes used to reconstruct matrices on demand.
        GroundcoverInstanceMatrices(osg::Vec4Array* transforms, osg::Vec3Array* rotations);

        /// Return the number of represented instances without materializing their matrices.
        std::size_t size() const noexcept;

        /// Return the lazily materialized instance matrices.
        const std::vector<osg::Matrix>& get() const;

    private:
        osg::ref_ptr<const osg::Vec4Array> mTransforms;
        osg::ref_ptr<const osg::Vec3Array> mRotations;
        mutable std::once_flag mOnce;
        mutable std::vector<osg::Matrix> mMatrices;
    };

    /// Refine groundcover primitive near/far bounds while avoiding unused matrix storage for other camera modes.
    class GroundcoverNearFarCullCallback final : public osg::DrawableCullCallback
    {
    public:
        /// Retain compact instance attributes and the untransformed model bound.
        GroundcoverNearFarCullCallback(
            osg::Vec4Array* transforms, osg::Vec3Array* rotations, const osg::BoundingBox& instanceBounds);

        /// Cull the whole drawable and refine primitive near/far bounds when the active camera requests it.
        bool cull(osg::NodeVisitor* visitor, osg::Drawable* drawable, osg::RenderInfo* renderInfo) const override;

    private:
        GroundcoverInstanceMatrices mInstanceMatrices;
        osg::BoundingBox mInstanceBounds;
    };
}

#endif
