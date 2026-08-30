#include "diagnosticcompositemapwork.hpp"

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>

#include <components/sceneutil/diagnosticgputiming.hpp>

#include <string>

namespace Terrain
{
    namespace
    {
        /// Resolve the camera that owns active bounded composite-map statistics.
        osg::Camera* diagnosticCamera(osg::RenderInfo& renderInfo)
        {
            osg::Camera* camera = renderInfo.getCurrentCamera();
            if (camera == nullptr || camera->getStats() == nullptr
                || !SceneUtil::diagnosticGpuPassStatisticsCollectionEnabled(*camera))
            {
                return nullptr;
            }
            return camera;
        }
    }

    bool diagnosticCompositeMapWorkCollectionEnabled(osg::RenderInfo& renderInfo)
    {
        return diagnosticCamera(renderInfo) != nullptr;
    }

    void recordDiagnosticCompositeMapWork(osg::RenderInfo& renderInfo, const DiagnosticCompositeMapWork& work)
    {
        osg::Camera* camera = diagnosticCamera(renderInfo);
        osg::State* state = renderInfo.getState();
        if (camera == nullptr || state == nullptr || state->getFrameStamp() == nullptr)
        {
            return;
        }

        osg::Stats& stats = *camera->getStats();
        const unsigned frame = state->getFrameStamp()->getFrameNumber();
        stats.setAttribute(frame, std::string(diagnosticCompositeMapBudgetStatistic), work.mBudgetMilliseconds);
        stats.setAttribute(frame, std::string(diagnosticCompositeMapQueuedBeforeStatistic), work.mQueuedBefore);
        stats.setAttribute(frame, std::string(diagnosticCompositeMapImmediateBeforeStatistic), work.mImmediateBefore);
        stats.setAttribute(frame, std::string(diagnosticCompositeMapProcessedStatistic), work.mProcessed);
        stats.setAttribute(
            frame, std::string(diagnosticCompositeMapSubmittedDrawablesStatistic), work.mSubmittedDrawables);
        stats.setAttribute(frame, std::string(diagnosticCompositeMapCompletedStatistic), work.mCompleted);
        stats.setAttribute(frame, std::string(diagnosticCompositeMapQueuedAfterStatistic), work.mQueuedAfter);
        stats.setAttribute(
            frame, std::string(diagnosticCompositeMapDrawableCpuStatistic), work.mDrawableCpuMilliseconds);
    }
}
