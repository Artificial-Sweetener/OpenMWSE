#ifndef OPENMW_MWRENDER_GROUNDCOVERLOD_H
#define OPENMW_MWRENDER_GROUNDCOVERLOD_H

#include <osg/ref_ptr>

namespace osg
{
    class Node;
}

namespace MWRender
{
    /// Return the distance beyond which the far groundcover representation is safe to use.
    float getGroundcoverFarLodDistance(float viewDistance);

    /// Clone a detailed groundcover model into a spatially representative reduced-triangle far model.
    osg::ref_ptr<osg::Node> makeGroundcoverFarLod(const osg::Node& detailedModel, float triangleRatio);
}

#endif
