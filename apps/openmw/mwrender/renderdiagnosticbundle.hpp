#ifndef OPENMW_MWRENDER_RENDERDIAGNOSTICBUNDLE_H
#define OPENMW_MWRENDER_RENDERDIAGNOSTICBUNDLE_H

#include "renderdiagnosticimage.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace MWRender
{
    inline constexpr std::uint32_t renderDiagnosticBundleVersion = 1;

    /// Stream bounded render attachments and publish a complete bundle after its final frame.
    class RenderDiagnosticBundleWriter
    {
    public:
        /// Open a private partial bundle, rejecting existing output paths.
        explicit RenderDiagnosticBundleWriter(const std::filesystem::path& path);
        /// Remove an unpublished partial bundle on cancellation or failure.
        ~RenderDiagnosticBundleWriter();
        /// Append one frame's attachments without retaining earlier pixels in memory.
        void append(std::span<const RenderDiagnosticImage> images);
        /// Seal the attachment count and publish the completed file by rename.
        void finish(std::uint64_t frameNumber);

    private:
        std::filesystem::path mPath;
        std::filesystem::path mPartialPath;
        std::ofstream mStream;
        std::uint32_t mImageCount = 0;
        bool mPublished = false;
    };

    /// Persist named render attachments in the versioned graphics-provider bundle format.
    void writeRenderDiagnosticBundle(
        const std::filesystem::path& path, std::uint64_t frameNumber, std::span<const RenderDiagnosticImage> images);
}

#endif
