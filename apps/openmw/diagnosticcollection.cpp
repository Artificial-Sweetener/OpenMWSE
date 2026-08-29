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

namespace OMW
{
    namespace
    {
        constexpr unsigned renderStatisticsLatency = 8;
        constexpr std::size_t retainedFrameCount = 1024;
    }

    DiagnosticCollection::DiagnosticCollection(osgViewer::Viewer& viewer)
        : mViewer(viewer)
    {
        osg::Stats* viewerStats = mViewer.getViewerStats();
        viewerStats->allocate(retainedFrameCount);
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

    bool DiagnosticCollection::start(std::uint64_t requestId, std::uint32_t sampleCount, unsigned currentFrame)
    {
        if (mPending.has_value())
            return false;
        if (sampleCount == 0 || sampleCount > 600)
            throw std::invalid_argument("diagnostic performance sample count is outside its bounds");
        const std::uint64_t lastFrame = static_cast<std::uint64_t>(currentFrame) + sampleCount - 1;
        const std::uint64_t completionFrame = lastFrame + renderStatisticsLatency;
        if (completionFrame > std::numeric_limits<unsigned>::max())
            throw std::overflow_error("diagnostic performance frame interval overflows");

        beginCollection();
        mPending = CompletedDiagnosticCollection{
            .mRequestId = requestId,
            .mFirstFrame = currentFrame,
            .mLastFrame = static_cast<unsigned>(lastFrame),
        };
        mCompletionFrame = static_cast<unsigned>(completionFrame);
        return true;
    }

    std::optional<CompletedDiagnosticCollection> DiagnosticCollection::update(unsigned currentFrame)
    {
        if (!mPending.has_value() || currentFrame < mCompletionFrame)
            return std::nullopt;
        CompletedDiagnosticCollection completed = *mPending;
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
        for (const char* category : { "engine", "resource", "frame_rate", "event", "update", "scene" })
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
        SceneUtil::setDiagnosticGpuPassStatisticsCollection(true);
    }

    void DiagnosticCollection::endCollection()
    {
        SceneUtil::setDiagnosticGpuPassStatisticsCollection(false);
        for (auto category = mEnabledCategories.rbegin(); category != mEnabledCategories.rend(); ++category)
        {
            if (category->mStats != nullptr)
                category->mStats->collectStats(category->mCategory, false);
        }
        mEnabledCategories.clear();
    }
}
