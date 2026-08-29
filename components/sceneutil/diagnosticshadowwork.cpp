#include "diagnosticshadowwork.hpp"

#include "diagnosticgputiming.hpp"

#include <osg/Camera>
#include <osg/Stats>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <cstddef>
#include <string>

namespace SceneUtil
{
    namespace
    {
        constexpr std::string_view shadowWorkFrameKey = "OpenMW diagnostic shadow work frame";
    }

    void setDiagnosticShadowWorkFrame(osg::Camera& camera, unsigned frame)
    {
        camera.setUserValue(std::string(shadowWorkFrameKey), frame);
    }

    DiagnosticShadowWorkRecorder::DiagnosticShadowWorkRecorder(osgUtil::RenderBin& renderBin) noexcept
        : mRenderBin(renderBin)
    {
    }

    DiagnosticShadowWorkRecorder::~DiagnosticShadowWorkRecorder()
    {
        osgUtil::RenderStage* stage = mRenderBin.getStage();
        osg::Camera* camera = stage != nullptr ? stage->getCamera() : nullptr;
        if (camera == nullptr || !diagnosticGpuPassStatisticsCollectionEnabled(*camera))
            return;

        osg::Stats* stats = camera->getStats();
        unsigned frame = 0;
        if (stats == nullptr || !camera->getUserValue(std::string(shadowWorkFrameKey), frame))
            return;

        std::size_t renderLeafCount = mRenderBin.getRenderLeafList().size();
        for (const osgUtil::StateGraph* stateGraph : mRenderBin.getStateGraphList())
            renderLeafCount += stateGraph->_leaves.size();

        stats->setAttribute(frame, std::string(diagnosticShadowRenderLeafCountStatistic),
            static_cast<double>(renderLeafCount));
        stats->setAttribute(frame, std::string(diagnosticShadowStateGraphCountStatistic),
            static_cast<double>(mRenderBin.getStateGraphList().size()));
    }
}
