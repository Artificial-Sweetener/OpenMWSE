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
    void RenderFrameDiagnostics::requestCapture(std::filesystem::path outputPath, std::uint32_t frameCount)
    {
        if (frameCount == 0 || frameCount > 600)
            throw std::invalid_argument("render frame diagnostic count must be within [1, 600]");

        std::lock_guard lock(mMutex);
        if (mOutputPath || mWriting)
            throw std::logic_error("render frame diagnostic capture is already in progress");
        mOutputPath = std::move(outputPath);
        mRequestedFrameCount = frameCount;
        mCapturedFrameCount = 0;
        mLastFrameNumber.reset();
        mImages.clear();
        mImages.reserve(frameCount * 4);
    }

    bool RenderFrameDiagnostics::captureRequested() const
    {
        std::lock_guard lock(mMutex);
        return mOutputPath.has_value() && mCapturedFrameCount < mRequestedFrameCount;
    }

    bool RenderFrameDiagnostics::captureInProgress() const
    {
        std::lock_guard lock(mMutex);
        return mOutputPath.has_value() || mWriting;
    }

    void RenderFrameDiagnostics::captureFrame(
        osg::RenderInfo& renderInfo, unsigned int framebuffer, int width, int height)
    {
        const std::uint64_t frameNumber = renderInfo.getState()->getFrameStamp()->getFrameNumber();
        std::size_t frameOrdinal = 0;
        {
            std::lock_guard lock(mMutex);
            if (!mOutputPath || mCapturedFrameCount >= mRequestedFrameCount || mLastFrameNumber == frameNumber)
                return;
            mLastFrameNumber = frameNumber;
            frameOrdinal = mCapturedFrameCount;
        }

        try
        {
            constexpr unsigned maximumCaptureDimension = 1024;
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

            std::optional<std::filesystem::path> completedPath;
            std::vector<RenderDiagnosticImage> completedImages;
            {
                std::lock_guard lock(mMutex);
                if (!mOutputPath)
                    return;
                mImages.emplace_back(std::move(image));
                mImages.insert(mImages.end(), std::make_move_iterator(shadowMaps.mImages.begin()),
                    std::make_move_iterator(shadowMaps.mImages.end()));
                ++mCapturedFrameCount;
                if (mCapturedFrameCount == mRequestedFrameCount)
                {
                    completedPath = std::move(mOutputPath);
                    mOutputPath.reset();
                    completedImages = std::move(mImages);
                    mWriting = true;
                }
            }

            if (completedPath)
            {
                std::filesystem::create_directories(completedPath->parent_path());
                writeRenderDiagnosticBundle(*completedPath, frameNumber, completedImages);
                Log(Debug::Info) << "Captured exact render-frame sequence to " << *completedPath;
                std::lock_guard lock(mMutex);
                mWriting = false;
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
            mWriting = false;
            mImages.clear();
        }
    }
}
