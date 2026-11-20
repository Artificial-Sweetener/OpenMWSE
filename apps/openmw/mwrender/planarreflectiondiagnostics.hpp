#ifndef OPENMW_MWRENDER_PLANARREFLECTIONDIAGNOSTICS_H
#define OPENMW_MWRENDER_PLANARREFLECTIONDIAGNOSTICS_H

#include "renderdiagnosticimage.hpp"

#include <vector>

namespace osg
{
    class RenderInfo;
    class Texture;
}

namespace MWRender
{
    class RenderDiagnosticCapture;

    /// Retain the dominant reflection attachments completed for one graphics context and frame.
    void publishDominantPlanarReflection(
        unsigned int context, unsigned int frame, osg::Texture& color, osg::Texture& depth);

    /// Retain the strongest non-dominant reflection attachments for reconstruction analysis.
    void publishComparisonPlanarReflection(
        unsigned int context, unsigned int frame, osg::Texture& color, osg::Texture& depth);

    /// Capture dominant parity and representative non-dominant attachments for the current render frame.
    std::vector<RenderDiagnosticImage> capturePlanarReflectionDiagnostics(
        RenderDiagnosticCapture& capture, osg::RenderInfo& renderInfo);
}

#endif
