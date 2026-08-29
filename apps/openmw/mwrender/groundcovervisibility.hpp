#ifndef OPENMW_MWRENDER_GROUNDCOVERVISIBILITY_H
#define OPENMW_MWRENDER_GROUNDCOVERVISIBILITY_H

#include <cstddef>
#include <mutex>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrix>
#include <osg/Referenced>

namespace osg
{
    class State;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace MWRender
{
    /// Identify a contiguous range in a spatially ordered groundcover instance array.
    struct GroundcoverInstanceRange
    {
        std::size_t mFirst = 0;
        std::size_t mCount = 0;

        /// Compare exact source-array coverage.
        bool operator==(const GroundcoverInstanceRange&) const = default;
    };

    /// Pair one spatial instance range with its animation-safe local bound.
    struct GroundcoverDrawCluster
    {
        GroundcoverInstanceRange mInstances;
        osg::BoundingBox mBounds;
    };

    /// Calculate and cache exact visible instance ranges for one model batch.
    class GroundcoverVisibility final : public osg::Referenced
    {
    public:
        /// Retain immutable cluster bounds and the exact configured view distance.
        GroundcoverVisibility(std::vector<GroundcoverDrawCluster> clusters, float viewDistance);

        /// Calculate or reuse visible ranges for one camera traversal.
        std::size_t update(osgUtil::CullVisitor& visitor, unsigned int contextId) const;

        /// Return the visible ranges matching the matrices currently applied for drawing.
        bool getVisibleRanges(const osg::State& state, std::vector<GroundcoverInstanceRange>& result) const;

        /// Return the complete instance count represented by the immutable cluster set.
        std::size_t totalInstances() const;

    private:
        /// Retain one bounded camera-state result for later draw traversal.
        struct VisibilityRecord
        {
            osg::Matrix mModelView;
            osg::Matrix mProjection;
            unsigned int mContextId = 0;
            std::size_t mVisibleInstances = 0;
            std::vector<GroundcoverInstanceRange> mVisibleRanges;
        };

        std::vector<GroundcoverDrawCluster> mClusters;
        float mViewDistance;
        mutable std::mutex mMutex;
        mutable std::vector<VisibilityRecord> mRecords;
    };
}

#endif
