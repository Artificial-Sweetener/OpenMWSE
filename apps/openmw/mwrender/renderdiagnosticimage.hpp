#ifndef OPENMW_MWRENDER_RENDERDIAGNOSTICIMAGE_H
#define OPENMW_MWRENDER_RENDERDIAGNOSTICIMAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace MWRender
{
    /// Holds one named floating-point render attachment before diagnostic serialization.
    struct RenderDiagnosticImage
    {
        std::string mName;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::uint32_t mChannels = 0;
        std::vector<float> mPixels;
    };
}

#endif
