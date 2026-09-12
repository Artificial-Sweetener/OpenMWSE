#ifndef OPENMW_MWRENDER_RENDERFRAMEDIAGNOSTICS_H
#define OPENMW_MWRENDER_RENDERFRAMEDIAGNOSTICS_H

#include "renderdiagnosticbundle.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace osg
{
    class RenderInfo;
}

namespace MWRender
{
    class ShadowMapDiagnosticMetadataWriter;

    /// Own a bounded sequence of exact production-frame composites and their submitted shadow maps.
    class RenderFrameDiagnostics
    {
    public:
        /// Initialize an idle frame-sequence capture without opening diagnostic artifacts.
        RenderFrameDiagnostics();

        /// Clean up unpublished frame and metadata streams after cancellation.
        ~RenderFrameDiagnostics();

        /// Stream a bounded sequence beginning at the scheduled route boundary, excluding older queued draw work.
        void requestCapture(std::filesystem::path outputPath, std::uint32_t frameCount, bool nativeResolution,
            std::uint64_t firstFrameNumber);

        /// Report whether the render thread should capture the current frame.
        bool captureRequested() const;

        /// Report whether a requested sequence is being captured or serialized.
        bool captureInProgress() const;

        /// Capture one eligible final composite and its shadow maps at most once for the current OSG frame.
        void captureFrame(osg::RenderInfo& renderInfo, unsigned int framebuffer, int width, int height);

    private:
        mutable std::mutex mMutex;
        std::optional<std::filesystem::path> mOutputPath;
        std::optional<std::uint64_t> mLastFrameNumber;
        std::uint32_t mRequestedFrameCount = 0;
        std::uint32_t mCapturedFrameCount = 0;
        std::uint64_t mFirstFrameNumber = 0;
        bool mNativeResolution = false;
        std::unique_ptr<RenderDiagnosticBundleWriter> mWriter;
        std::unique_ptr<ShadowMapDiagnosticMetadataWriter> mShadowMetadataWriter;
    };
}

#endif
