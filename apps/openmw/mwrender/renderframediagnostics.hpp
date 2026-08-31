#ifndef OPENMW_MWRENDER_RENDERFRAMEDIAGNOSTICS_H
#define OPENMW_MWRENDER_RENDERFRAMEDIAGNOSTICS_H

#include "renderdiagnosticimage.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace osg
{
    class RenderInfo;
}

namespace MWRender
{
    /// Own a bounded sequence of exact production-frame composites and their submitted shadow maps.
    class RenderFrameDiagnostics
    {
    public:
        /// Schedule the requested number of rendered frames into one versioned diagnostic bundle.
        void requestCapture(std::filesystem::path outputPath, std::uint32_t frameCount);

        /// Report whether the render thread should capture the current frame.
        bool captureRequested() const;

        /// Report whether a requested sequence is being captured or serialized.
        bool captureInProgress() const;

        /// Capture one final composite and its shadow maps at most once for the current OSG frame.
        void captureFrame(osg::RenderInfo& renderInfo, unsigned int framebuffer, int width, int height);

    private:
        mutable std::mutex mMutex;
        std::optional<std::filesystem::path> mOutputPath;
        std::optional<std::uint64_t> mLastFrameNumber;
        std::uint32_t mRequestedFrameCount = 0;
        std::uint32_t mCapturedFrameCount = 0;
        bool mWriting = false;
        std::vector<RenderDiagnosticImage> mImages;
    };
}

#endif
