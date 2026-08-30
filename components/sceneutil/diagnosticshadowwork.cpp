#include "diagnosticshadowwork.hpp"

#include "diagnosticgputiming.hpp"

#include <osg/Camera>
#include <osg/Stats>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <cmath>
#include <cstddef>
#include <string>

namespace SceneUtil
{
    namespace
    {
        constexpr std::string_view shadowWorkFrameKey = "OpenMW diagnostic shadow work frame";
        constexpr std::string_view shadowLightVerticalMagnitudeKey
            = "OpenMW diagnostic shadow light vertical magnitude value";
        constexpr std::string_view shadowLightLuminanceKey = "OpenMW diagnostic shadow light luminance value";
    }

    void setDiagnosticShadowLightState(
        osg::Camera& camera, const osg::Vec3d& lightDirection, const osg::Vec4f& diffuseColor)
    {
        const double directionLength = lightDirection.length();
        if (!std::isfinite(directionLength) || directionLength <= 1e-8)
            return;

        camera.setUserValue(
            std::string(shadowLightVerticalMagnitudeKey), std::abs(lightDirection.z()) / directionLength);
        camera.setUserValue(std::string(shadowLightLuminanceKey),
            static_cast<double>(diffuseColor.x()) * 0.2126 + static_cast<double>(diffuseColor.y()) * 0.7152
                + static_cast<double>(diffuseColor.z()) * 0.0722);
    }

    void setDiagnosticShadowWorkFrame(osg::Camera& camera, unsigned frame)
    {
        camera.setUserValue(std::string(shadowWorkFrameKey), frame);
        if (!diagnosticGpuPassStatisticsCollectionEnabled(camera))
            return;

        osg::Stats* stats = camera.getStats();
        if (stats == nullptr)
            return;

        double value = 0.0;
        if (camera.getUserValue(std::string(shadowLightVerticalMagnitudeKey), value))
        {
            stats->setAttribute(frame, std::string(diagnosticShadowLightVerticalMagnitudeStatistic), value);
        }
        if (camera.getUserValue(std::string(shadowLightLuminanceKey), value))
            stats->setAttribute(frame, std::string(diagnosticShadowLightLuminanceStatistic), value);
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
