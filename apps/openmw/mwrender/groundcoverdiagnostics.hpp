#ifndef OPENMW_MWRENDER_GROUNDCOVERDIAGNOSTICS_H
#define OPENMW_MWRENDER_GROUNDCOVERDIAGNOSTICS_H

#include "groundcovervisibility.hpp"

#include <span>

namespace osgUtil
{
    class CullVisitor;
}

namespace MWRender
{
    /// Measure conservative per-instance groundcover visibility only while scene diagnostics are active.
    void recordDiagnosticGroundcoverVisibility(osgUtil::CullVisitor& visitor, const osg::Vec4Array& transforms,
        float modelExtent, float viewDistance, std::span<const GroundcoverInstanceRange> visibleClusterRanges);
}

#endif
