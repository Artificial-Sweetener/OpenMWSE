#ifndef OPENMW_MWRENDER_GROUNDCOVERBATCHING_H
#define OPENMW_MWRENDER_GROUNDCOVERBATCHING_H

#include "groundcovervisibility.hpp"

#include <cstddef>
#include <vector>

#include <osg/Referenced>
#include <osg/ref_ptr>

namespace osg
{
    class Geometry;
    class Node;
    class State;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace MWRender
{
    /// Submit one model's spatially visible groundcover ranges through exact indirect batches.
    class GroundcoverBatch final : public osg::Referenced
    {
    public:
        /// Create range submission for immutable instances with a shared live distance owner.
        GroundcoverBatch(std::vector<GroundcoverDrawCluster> clusters, osg::Vec4Array* transforms, float modelExtent,
            GroundcoverDistance* distance);

        /// Replace supported instanced primitive sets and register the geometry with this model batch.
        bool configure(osg::Geometry& geometry);

        /// Attach one model-owned visibility boundary while preserving the node's existing cull callback.
        void attach(osg::Node& node);

        /// Cull clusters once for a model and report the exact work submitted by all registered drawables.
        bool cull(osgUtil::CullVisitor& visitor) const;

        /// Return the visible ranges matching the matrices currently applied for drawing.
        bool getVisibleRanges(const osg::State& state, std::vector<GroundcoverInstanceRange>& result) const;

    private:
        osg::ref_ptr<GroundcoverVisibility> mVisibility;
        std::size_t mDrawableCount = 0;
        bool mRangeDrawingCompatible = true;
        bool mNeedsArrayDrawing = false;
        bool mNeedsElementDrawing = false;
    };
}

#endif
