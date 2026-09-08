#include "renderframediagnostics.hpp"

#include "renderdiagnosticbundle.hpp"
#include "renderdiagnosticcapture.hpp"
#include "shadowmapdiagnostics.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <osg/FrameStamp>
#include <osg/RenderInfo>
#include <osg/State>

#include <components/debug/debuglog.hpp>

namespace MWRender
{
    void RenderFrameDiagnostics::requestCapture(std::filesystem::path outputPath, std::uint32_t frameCount,
        bool nativeResolution, std::uint64_t firstFrameNumber)
    {
        if (frameCount == 0 || frameCount > 600)
            throw std::invalid_argument("render frame diagnostic count must be within [1, 600]");

        std::lock_guard lock(mMutex);
        if (mOutputPath)
            throw std::logic_error("render frame diagnostic capture is already in progress");
        mWriter = std::make_unique<RenderDiagnosticBundleWriter>(outputPath);
        mOutputPath = std::move(outputPath);
        mNativeResolution = nativeResolution;
        mRequestedFrameCount = frameCount;
        mCapturedFrameCount = 0;
        mFirstFrameNumber = firstFrameNumber;
        mLastFrameNumber.reset();
    }

    bool RenderFrameDiagnostics::captureRequested() const
    {
        std::lock_guard lock(mMutex);
        return mOutputPath.has_value() && mCapturedFrameCount < mRequestedFrameCount;
    }

    bool RenderFrameDiagnostics::captureInProgress() const
    {
        std::lock_guard lock(mMutex);
        return mOutputPath.has_value();
    }

    void RenderFrameDiagnostics::captureFrame(
        osg::RenderInfo& renderInfo, unsigned int framebuffer, int width, int height)
    {
        const std::uint64_t frameNumber = renderInfo.getState()->getFrameStamp()->getFrameNumber();
        std::size_t frameOrdinal = 0;
        {
            std::lock_guard lock(mMutex);
            if (!mOutputPath || frameNumber < mFirstFrameNumber || mCapturedFrameCount >= mRequestedFrameCount
                || mLastFrameNumber == frameNumber)
                return;
            mLastFrameNumber = frameNumber;
            frameOrdinal = mCapturedFrameCount;
        }

        try
        {
            if (mNativeResolution
                && (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 64u * 1024u * 1024u))
                throw std::runtime_error("native-resolution frame capture exceeds the 64-million-pixel frame budget");
            const unsigned maximumCaptureDimension = mNativeResolution ? 0 : 1024;
            std::ostringstream name;
            name << "route-frame-" << std::setw(4) << std::setfill('0') << frameOrdinal << "-osg-" << frameNumber;
            RenderDiagnosticCapture capture(renderInfo);
            RenderDiagnosticImage image = capture.captureFramebuffer(
                framebuffer, width, height, name.str(), maximumCaptureDimension);
            ShadowMapDiagnosticCapture shadowMaps = captureShadowMapDiagnostics(capture, renderInfo, 512);
            for (RenderDiagnosticImage& shadowMap : shadowMaps.mImages)
            {
                std::ostringstream shadowName;
                shadowName << "route-shadow-frame-" << std::setw(4) << std::setfill('0') << frameOrdinal << '-'
                           << shadowMap.mName;
                shadowMap.mName = shadowName.str();
            }

            std::lock_guard lock(mMutex);
            if (!mOutputPath)
                return;
            mWriter->append(std::span<const RenderDiagnosticImage>(&image, 1));
            mWriter->append(shadowMaps.mImages);
            ++mCapturedFrameCount;
            if (mCapturedFrameCount == mRequestedFrameCount)
            {
                mWriter->finish(frameNumber);
                Log(Debug::Info) << "Captured exact render-frame sequence to " << *mOutputPath;
                mWriter.reset();
                mOutputPath.reset();
                mRequestedFrameCount = 0;
                mCapturedFrameCount = 0;
                mLastFrameNumber.reset();
            }
        }
        catch (const std::exception& exception)
        {
            Log(Debug::Error) << "Failed to capture exact render-frame sequence: " << exception.what();
            std::lock_guard lock(mMutex);
            mOutputPath.reset();
            mRequestedFrameCount = 0;
            mCapturedFrameCount = 0;
            mLastFrameNumber.reset();
            mWriter.reset();
        }
    }
}
