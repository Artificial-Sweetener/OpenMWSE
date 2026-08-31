#include "diagnosticshadowwork.hpp"

#include "diagnosticgputiming.hpp"

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>
#include <osg/Texture2D>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace SceneUtil
{
    namespace
    {
        constexpr std::string_view shadowWorkFrameKey = "OpenMW diagnostic shadow work frame";
        constexpr std::string_view shadowLightVerticalMagnitudeKey
            = "OpenMW diagnostic shadow light vertical magnitude value";
        constexpr std::string_view shadowLightLuminanceKey = "OpenMW diagnostic shadow light luminance value";
        constexpr std::string_view shadowCasterCullMinimumKey = "OpenMW diagnostic shadow caster cull minimum value";
        constexpr std::string_view shadowReceiverDepthSpanKey = "OpenMW diagnostic shadow receiver depth span value";
        constexpr std::string_view shadowCascadeNearKey = "OpenMW diagnostic shadow cascade near";
        constexpr std::string_view shadowCascadeFarKey = "OpenMW diagnostic shadow cascade far";
        constexpr std::array<std::string_view, 6> shadowReceiverCropKeys{
            "OpenMW diagnostic shadow receiver x minimum",
            "OpenMW diagnostic shadow receiver x maximum",
            "OpenMW diagnostic shadow receiver y minimum",
            "OpenMW diagnostic shadow receiver y maximum",
            "OpenMW diagnostic shadow receiver z minimum",
            "OpenMW diagnostic shadow receiver z maximum",
        };
        constexpr unsigned retainedShadowMapFrames = 8;

        std::mutex shadowMapMutex;
        std::map<std::pair<unsigned, unsigned>, std::vector<DiagnosticShadowMap>> renderedShadowMaps;

        /// Publishes the texture and transforms after one shadow camera finishes drawing.
        class ShadowMapCaptureCallback final : public osg::Camera::DrawCallback
        {
        public:
            explicit ShadowMapCaptureCallback(osg::Texture2D& texture)
                : mTexture(&texture)
            {
            }

            /// Record the completed shadow target without reading GPU pixels until diagnostics request them.
            void operator()(osg::RenderInfo& renderInfo) const override
            {
                osg::State* state = renderInfo.getState();
                osg::Camera* camera = renderInfo.getCurrentCamera();
                if (state == nullptr || state->getFrameStamp() == nullptr || camera == nullptr)
                    return;
                if (!diagnosticGpuPassStatisticsCollectionEnabled(*camera))
                    return;

                DiagnosticShadowMap map;
                map.mFrame = state->getFrameStamp()->getFrameNumber();
                map.mContext = state->getContextID();
                map.mCascade = diagnosticGpuPassInstance(*camera).value_or(0);
                map.mTexture = mTexture;
                map.mViewMatrix = camera->getViewMatrix();
                map.mProjectionMatrix = camera->getProjectionMatrix();
                camera->getUserValue(std::string(shadowCascadeNearKey), map.mCascadeNear);
                camera->getUserValue(std::string(shadowCascadeFarKey), map.mCascadeFar);
                for (std::size_t index = 0; index < shadowReceiverCropKeys.size(); ++index)
                    camera->getUserValue(std::string(shadowReceiverCropKeys[index]), map.mReceiverCrop[index]);

                std::lock_guard lock(shadowMapMutex);
                const unsigned renderedFrame = map.mFrame;
                renderedShadowMaps[{ map.mContext, map.mFrame }].push_back(std::move(map));
                std::erase_if(renderedShadowMaps, [context = state->getContextID(), renderedFrame](const auto& entry) {
                    return entry.first.first == context && renderedFrame > entry.first.second
                        && renderedFrame - entry.first.second > retainedShadowMapFrames;
                });
            }

        private:
            osg::ref_ptr<osg::Texture2D> mTexture;
        };
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

    void enableDiagnosticShadowMapCapture(osg::Camera& camera, osg::Texture2D& texture)
    {
        camera.addFinalDrawCallback(new ShadowMapCaptureCallback(texture));
    }

    void setDiagnosticShadowProjectionState(
        osg::Camera& camera, double cascadeNear, double cascadeFar, const std::array<double, 6>& receiverCrop)
    {
        if (!diagnosticGpuPassStatisticsCollectionEnabled(camera))
            return;
        camera.setUserValue(std::string(shadowCascadeNearKey), cascadeNear);
        camera.setUserValue(std::string(shadowCascadeFarKey), cascadeFar);
        for (std::size_t index = 0; index < shadowReceiverCropKeys.size(); ++index)
            camera.setUserValue(std::string(shadowReceiverCropKeys[index]), receiverCrop[index]);
    }

    void setDiagnosticShadowCasterCullState(osg::Camera& camera, double receiverMinimum, double receiverMaximum)
    {
        if (!diagnosticGpuPassStatisticsCollectionEnabled(camera))
            return;
        camera.setUserValue(std::string(shadowCasterCullMinimumKey), receiverMinimum);
        camera.setUserValue(std::string(shadowReceiverDepthSpanKey), receiverMaximum - receiverMinimum);
    }

    std::vector<DiagnosticShadowMap> diagnosticShadowMaps(unsigned context, unsigned frame)
    {
        std::lock_guard lock(shadowMapMutex);
        const auto found = renderedShadowMaps.find({ context, frame });
        return found != renderedShadowMaps.end() ? found->second : std::vector<DiagnosticShadowMap>{};
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
        if (camera.getUserValue(std::string(shadowCasterCullMinimumKey), value))
            stats->setAttribute(frame, std::string(diagnosticShadowCasterCullMinimumStatistic), value);
        if (camera.getUserValue(std::string(shadowReceiverDepthSpanKey), value))
            stats->setAttribute(frame, std::string(diagnosticShadowReceiverDepthSpanStatistic), value);
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

        stats->setAttribute(
            frame, std::string(diagnosticShadowRenderLeafCountStatistic), static_cast<double>(renderLeafCount));
        stats->setAttribute(frame, std::string(diagnosticShadowStateGraphCountStatistic),
            static_cast<double>(mRenderBin.getStateGraphList().size()));
    }
}
