#ifndef OPENMW_MWRENDER_RENDERDIAGNOSTICBUNDLE_H
#define OPENMW_MWRENDER_RENDERDIAGNOSTICBUNDLE_H

#include "renderdiagnosticimage.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace MWRender
{
    inline constexpr std::uint32_t renderDiagnosticBundleVersion = 1;

    /// Persist named render attachments in the versioned graphics-provider bundle format.
    void writeRenderDiagnosticBundle(
        const std::filesystem::path& path, std::uint64_t frameNumber, std::span<const RenderDiagnosticImage> images);
}

#endif
