#include "groundcovervisibility.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <osg/State>
#include <osgUtil/CullVisitor>

#include <components/terrain/quadtreenode.hpp>

namespace MWRender
{
    GroundcoverVisibility::GroundcoverVisibility(std::vector<GroundcoverDrawCluster> clusters, float viewDistance)
        : mClusters(std::move(clusters))
        , mViewDistance(viewDistance)
    {
        if (!std::isfinite(viewDistance) || viewDistance < 0.f)
            throw std::invalid_argument("groundcover view distance must be finite and non-negative");
    }

    std::size_t GroundcoverVisibility::update(osgUtil::CullVisitor& visitor, unsigned int contextId) const
    {
        const osg::Matrix modelView = *visitor.getModelViewMatrix();
        const osg::Matrix projection = *visitor.getProjectionMatrix();
        {
            std::scoped_lock lock(mMutex);
            const auto existing = std::find_if(mRecords.rbegin(), mRecords.rend(), [&](const VisibilityRecord& record) {
                return record.mContextId == contextId && record.mModelView == modelView
                    && record.mProjection == projection;
            });
            if (existing != mRecords.rend())
                return existing->mVisibleInstances;
        }

        std::vector<GroundcoverInstanceRange> visibleRanges;
        visibleRanges.reserve(mClusters.size());
        std::size_t visibleInstances = 0;
        for (const GroundcoverDrawCluster& cluster : mClusters)
        {
            if (Terrain::distance(cluster.mBounds, visitor.getEyePoint()) > mViewDistance
                || visitor.isCulled(cluster.mBounds))
                continue;
            visibleRanges.push_back(cluster.mInstances);
            visibleInstances += cluster.mInstances.mCount;
        }

        VisibilityRecord record{ modelView, projection, contextId, visibleInstances, std::move(visibleRanges) };
        std::scoped_lock lock(mMutex);
        const auto existing = std::find_if(mRecords.begin(), mRecords.end(), [&](const VisibilityRecord& candidate) {
            return candidate.mContextId == record.mContextId && candidate.mModelView == record.mModelView
                && candidate.mProjection == record.mProjection;
        });
        if (existing != mRecords.end())
            *existing = std::move(record);
        else
        {
            constexpr std::size_t maximumRecords = 8;
            if (mRecords.size() == maximumRecords)
                mRecords.erase(mRecords.begin());
            mRecords.push_back(std::move(record));
        }
        return visibleInstances;
    }

    bool GroundcoverVisibility::getVisibleRanges(
        const osg::State& state, std::vector<GroundcoverInstanceRange>& result) const
    {
        std::scoped_lock lock(mMutex);
        const auto record = std::find_if(mRecords.rbegin(), mRecords.rend(), [&](const VisibilityRecord& candidate) {
            return candidate.mContextId == state.getContextID() && candidate.mModelView == state.getModelViewMatrix()
                && candidate.mProjection == state.getProjectionMatrix();
        });
        if (record == mRecords.rend())
            return false;
        result = record->mVisibleRanges;
        return true;
    }

    std::size_t GroundcoverVisibility::totalInstances() const
    {
        std::size_t result = 0;
        for (const GroundcoverDrawCluster& cluster : mClusters)
            result += cluster.mInstances.mCount;
        return result;
    }
}
