#ifndef OPENMW_MWRENDER_GROUNDCOVERLOD_H
#define OPENMW_MWRENDER_GROUNDCOVERLOD_H

#include <osg/Group>

namespace osg
{
    class Node;
}

namespace MWRender
{
    class GroundcoverDistance;

    /// Select detailed or reduced grass geometry using the current shared distance without mutating node ranges.
    class GroundcoverLod final : public osg::Group
    {
    public:
        /// Create an empty node for OSG cloning and serialization.
        GroundcoverLod();
        /// Retain the distance owner shared by both child representations.
        explicit GroundcoverLod(GroundcoverDistance* distance);
        /// Clone children while retaining their distance owner.
        GroundcoverLod(const GroundcoverLod& other, const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY);
        /// Release the shared distance owner after node traversal has finished.
        ~GroundcoverLod() override;
        META_Node(MWRender, GroundcoverLod);
        /// Select one child for active traversal, preserving all-child inspection and OSG's LOD scale.
        void traverse(osg::NodeVisitor& visitor) override;

    private:
        osg::ref_ptr<GroundcoverDistance> mDistance;
    };

    /// Return the distance beyond which the far groundcover representation is safe to use.
    float getGroundcoverFarLodDistance(float viewDistance);

    /// Clone a detailed groundcover model into a spatially representative reduced-triangle far model.
    osg::ref_ptr<osg::Node> makeGroundcoverFarLod(const osg::Node& detailedModel, float triangleRatio);
}

#endif
