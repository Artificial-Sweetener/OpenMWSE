#include "diagnosticviewselection.hpp"

#include <components/sceneutil/diagnosticgputiming.hpp>

#include <osg/Camera>
#include <osg/Stats>

#include <string>

namespace Terrain
{
    bool diagnosticViewSelectionCollectionEnabled(const osg::Camera& camera)
    {
        return SceneUtil::diagnosticGpuPassStatisticsCollectionEnabled(camera);
    }

    void recordDiagnosticViewSelection(osg::Camera& camera, unsigned frame, const DiagnosticViewSelection& selection)
    {
        if (!diagnosticViewSelectionCollectionEnabled(camera))
            return;

        osg::Stats* stats = camera.getStats();
        if (stats == nullptr)
            return;

        stats->setAttribute(frame, std::string(diagnosticSelectedNodeCountStatistic), selection.mSelectedNodes);
        stats->setAttribute(frame, std::string(diagnosticActiveGridNodeCountStatistic), selection.mActiveGridNodes);
        stats->setAttribute(
            frame, std::string(diagnosticPendingRenderingNodeCountStatistic), selection.mPendingRenderingNodes);
        stats->setAttribute(frame, std::string(diagnosticViewWorldRevisionStatistic), selection.mWorldRevision);
        stats->setAttribute(
            frame, std::string(diagnosticViewSelectionUpdatedStatistic), selection.mSelectionUpdated ? 1.0 : 0.0);
        stats->setAttribute(
            frame, std::string(diagnosticViewSelectionChangedStatistic), selection.mSelectionChanged ? 1.0 : 0.0);
    }
}
