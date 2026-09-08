#include "diagnosticcollection.hpp"

#include "diagnosticrenderingtiming.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include <osg/Camera>
#include <osg/Node>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sdlutil/diagnosticpresentationtiming.hpp>

namespace OMW
{
    namespace
    {
        constexpr unsigned renderStatisticsLatency = SceneUtil::diagnosticGpuTimingReadbackLatencyFrames + 2;
    }

    DiagnosticCollection::DiagnosticCollection(osgViewer::Viewer& viewer)
        : mViewer(viewer)
    {
        osg::Stats* viewerStats = mViewer.getViewerStats();
        viewerStats->allocate(SceneUtil::diagnosticGpuTimingRetainedFrames);
        SceneUtil::enableDiagnosticGpuPassStatistics(*mViewer.getCamera());

        osgViewer::Viewer::Cameras cameras;
        mViewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            SceneUtil::enableDiagnosticGpuPassStatistics(*camera);
        if (osg::Node* scene = mViewer.getSceneData())
            SceneUtil::enableDiagnosticGpuPassStatistics(*scene);
        enableDiagnosticRenderBinTimings();
    }

    DiagnosticCollection::~DiagnosticCollection()
    {
        endCollection();
    }

    bool DiagnosticCollection::start(
        std::uint64_t requestId, std::uint32_t sampleCount, unsigned currentFrame, DiagnosticSampleBasis sampleBasis)
    {
        if (mPending.has_value())
            return false;
        if (sampleCount == 0 || sampleCount > 600)
            throw std::invalid_argument("diagnostic performance sample count is outside its bounds");
        const std::uint64_t lastFrame = static_cast<std::uint64_t>(currentFrame) + sampleCount - 1;
        const std::uint64_t completionFrame = lastFrame + renderStatisticsLatency;
        // Leave two frames for the draw thread before the oldest requested statistic can be overwritten.
        const std::uint64_t retirementLimit
            = static_cast<std::uint64_t>(currentFrame) + SceneUtil::diagnosticGpuTimingRetainedFrames - 2;
        if (retirementLimit > std::numeric_limits<unsigned>::max())
            throw std::overflow_error("diagnostic performance frame interval overflows");

        beginCollection();
        mPending = CompletedDiagnosticCollection{
            .mRequestId = requestId,
            .mFirstFrame = currentFrame,
            .mLastFrame = static_cast<unsigned>(lastFrame),
            .mRouteFrames = {},
            .mHistoryExpired = false,
        };
        mCompletionFrame = static_cast<unsigned>(completionFrame);
        mRetirementLimitFrame = static_cast<unsigned>(retirementLimit);
        mRequestedRouteFrames = sampleBasis == DiagnosticSampleBasis::GameplayRouteFrames ? sampleCount : 0;
        mPending->mRouteFrames.reserve(mRequestedRouteFrames);
        return true;
    }

    void DiagnosticCollection::recordRouteFrame(unsigned firstRenderFrame, unsigned sceneRenderFrame)
    {
        if (!mPending || mRequestedRouteFrames == 0 || mPending->mHistoryExpired)
            return;
        if (mPending->mRouteFrames.size() >= mRequestedRouteFrames)
            throw std::logic_error("diagnostic route collection received too many route frames");
        const unsigned expectedFirstFrame = mPending->mRouteFrames.empty()
            ? mPending->mFirstFrame
            : mPending->mRouteFrames.back().mSceneRenderFrame + 1;
        if (firstRenderFrame != expectedFirstFrame || sceneRenderFrame < firstRenderFrame)
            throw std::logic_error("diagnostic route collection received a discontinuous renderer interval");

        mPending->mRouteFrames.push_back({ firstRenderFrame, sceneRenderFrame });
        mPending->mLastFrame = sceneRenderFrame;
        const std::uint64_t completionFrame = static_cast<std::uint64_t>(sceneRenderFrame) + renderStatisticsLatency;
        if (completionFrame > mRetirementLimitFrame)
            mPending->mHistoryExpired = true;
        else
            mCompletionFrame = static_cast<unsigned>(completionFrame);
    }

    std::optional<CompletedDiagnosticCollection> DiagnosticCollection::update(unsigned currentFrame)
    {
        if (!mPending.has_value())
            return std::nullopt;
        if (mRequestedRouteFrames != 0 && currentFrame > mRetirementLimitFrame)
            mPending->mHistoryExpired = true;
        if (mRequestedRouteFrames != 0 && mPending->mRouteFrames.size() < mRequestedRouteFrames)
        {
            if (currentFrame >= mRetirementLimitFrame)
                mPending->mHistoryExpired = true;
            if (!mPending->mHistoryExpired)
                return std::nullopt;
        }
        if (!mPending->mHistoryExpired && currentFrame < mCompletionFrame)
            return std::nullopt;
        if (!mPending->mHistoryExpired && currentFrame < mRetirementLimitFrame
            && SceneUtil::diagnosticGpuQueriesPending(mPending->mFirstFrame, mPending->mLastFrame))
            return std::nullopt;
        CompletedDiagnosticCollection completed = std::move(*mPending);
        mPending.reset();
        endCollection();
        return completed;
    }

    bool DiagnosticCollection::active() const noexcept
    {
        return mPending.has_value();
    }

    void DiagnosticCollection::enable(osg::Stats& stats, std::string category)
    {
        if (stats.collectStats(category))
            return;
        stats.collectStats(category, true);
        mEnabledCategories.push_back({ &stats, std::move(category) });
    }

    void DiagnosticCollection::beginCollection()
    {
        osg::Stats* viewerStats = mViewer.getViewerStats();
        for (const char* category : { "engine", "resource", "frame_rate", "event", "update" })
            enable(*viewerStats, category);

        osg::Camera* rootCamera = mViewer.getCamera();
        SceneUtil::enableDiagnosticGpuPassStatistics(*rootCamera);
        enable(*rootCamera->getStats(), "rendering");
        enable(*rootCamera->getStats(), "scene");

        osgViewer::Viewer::Cameras cameras;
        mViewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
        {
            SceneUtil::enableDiagnosticGpuPassStatistics(*camera);
            enable(*camera->getStats(), "rendering");
            enable(*camera->getStats(), "scene");
        }
        if (osg::Node* scene = mViewer.getSceneData())
            SceneUtil::enableDiagnosticGpuPassStatistics(*scene);
        SDLUtil::setDiagnosticPresentationTimingStatistics(viewerStats);
        SceneUtil::setDiagnosticGpuPassStatisticsCollection(true);
    }

    void DiagnosticCollection::endCollection()
    {
        SDLUtil::setDiagnosticPresentationTimingStatistics(nullptr);
        SceneUtil::setDiagnosticGpuPassStatisticsCollection(false);
        for (auto category = mEnabledCategories.rbegin(); category != mEnabledCategories.rend(); ++category)
        {
            if (category->mStats != nullptr)
                category->mStats->collectStats(category->mCategory, false);
        }
        mEnabledCategories.clear();
    }
}
