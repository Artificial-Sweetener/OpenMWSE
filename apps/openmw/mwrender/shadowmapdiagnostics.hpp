#ifndef OPENMW_MWRENDER_SHADOWMAPDIAGNOSTICS_H
#define OPENMW_MWRENDER_SHADOWMAPDIAGNOSTICS_H

#include "renderdiagnosticimage.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
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

    /// Stream frame-indexed shadow projection metadata and publish it beside a completed route bundle.
    class ShadowMapDiagnosticMetadataWriter
    {
    public:
        /// Open a private partial sidecar and reject pre-existing diagnostic output.
        explicit ShadowMapDiagnosticMetadataWriter(const std::filesystem::path& path);

        /// Remove an unpublished partial sidecar after cancellation or failure.
        ~ShadowMapDiagnosticMetadataWriter();

        /// Append one route frame's render identity and complete shadow-map metadata document.
        void append(std::size_t routeOrdinal, std::uint64_t renderFrame, std::string_view metadata);

        /// Close and atomically publish the completed metadata sidecar.
        void finish();

    private:
        std::filesystem::path mPath;
        std::filesystem::path mPartialPath;
        std::ofstream mStream;
        bool mPublished = false;
    };
}

#endif
