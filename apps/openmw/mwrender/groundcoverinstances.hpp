#ifndef OPENMW_MWRENDER_GROUNDCOVERINSTANCES_H
#define OPENMW_MWRENDER_GROUNDCOVERINSTANCES_H

#include "groundcover.hpp"
#include "groundcovervisibility.hpp"

#include <cstddef>
#include <span>
#include <vector>

#include <osg/Array>
#include <osg/BoundingBox>
#include <osg/ref_ptr>

namespace MWRender
{
    /// Own the source-index order and cluster ranges used by one model batch.
    struct GroundcoverInstanceLayout
    {
        std::vector<std::size_t> mSourceIndices;
        std::vector<GroundcoverInstanceRange> mClusters;
    };

    /// Own immutable, spatially ordered attributes and bounds shared by one instanced model batch.
    struct GroundcoverInstanceBatch
    {
        osg::ref_ptr<osg::Vec4Array> mTransforms;
        osg::ref_ptr<osg::Vec3Array> mRotations;
        osg::BoundingBox mBounds;
        std::vector<GroundcoverDrawCluster> mClusters;
    };

    /// Group model instances into first-seen spatial clusters and return their contiguous draw order.
    GroundcoverInstanceLayout makeGroundcoverInstanceLayout(
        std::span<const Groundcover::GroundcoverEntry> instances, float clusterSize);

    /// Build spatially ordered instance attributes with model-wide, animation-safe cluster bounds.
    GroundcoverInstanceBatch makeGroundcoverInstanceBatch(std::span<const Groundcover::GroundcoverEntry> instances,
        const GroundcoverInstanceLayout& layout, const osg::Vec3f& batchPosition, float modelExtent);
}

#endif
