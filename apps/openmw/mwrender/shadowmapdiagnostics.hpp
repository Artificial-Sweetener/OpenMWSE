#ifndef OPENMW_MWRENDER_SHADOWMAPDIAGNOSTICS_H
#define OPENMW_MWRENDER_SHADOWMAPDIAGNOSTICS_H

#include "renderdiagnosticimage.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace osg
{
    class RenderInfo;
}

namespace MWRender
{
    class RenderDiagnosticCapture;

    /// Holds frame-matched shadow-map pixels and projection metadata for diagnostic serialization.
    struct ShadowMapDiagnosticCapture
    {
        std::vector<RenderDiagnosticImage> mImages;
        std::string mMetadata;
    };

    /// Capture bounded shadow-map evidence rendered by the current graphics context and frame.
    ShadowMapDiagnosticCapture captureShadowMapDiagnostics(
        RenderDiagnosticCapture& capture, osg::RenderInfo& renderInfo, unsigned maximumDimension);

    /// Persist shadow-map projection metadata alongside its render diagnostic bundle.
    void writeShadowMapDiagnosticMetadata(const std::filesystem::path& path, std::string_view metadata);
}

#endif
