#ifndef OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICGPUTIMING_H
#define OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICGPUTIMING_H

#include <osg/ref_ptr>

#include <span>
#include <string_view>
#include <vector>

namespace osg
{
    class Camera;
    class Node;
}

namespace osgUtil
{
    class RenderBin;
}

namespace SceneUtil
{
    /// Identifies one stable camera-owned GPU pass in diagnostic output.
    struct DiagnosticGpuPass
    {
        std::string_view mName;
        std::string_view mCameraName;
    };

    inline constexpr std::string_view sceneCameraName = "SceneCam";
    inline constexpr std::string_view shadowCameraName = "ShadowCamera";
    inline constexpr std::string_view reflectionCameraName = "ReflectionCamera";
    inline constexpr std::string_view skyCameraName = "SkyCamera";
    inline constexpr std::string_view postProcessingCameraName = "PostProcessingCamera";
    inline constexpr std::string_view precipitationOcclusionCameraName = "PrecipitationOcclusionCamera";
    inline constexpr std::string_view ripplesCameraName = "RipplesCamera";
    inline constexpr std::string_view diagnosticGpuPassTimeStatistic = "OpenMW diagnostic GPU pass time taken";
    inline constexpr std::string_view diagnosticCpuPassTimeStatistic = "OpenMW diagnostic CPU pass time taken";
    inline constexpr std::string_view diagnosticCpuPassBeginStatistic = "OpenMW diagnostic CPU pass begin time";
    inline constexpr std::string_view diagnosticCpuPassEndStatistic = "OpenMW diagnostic CPU pass end time";
    inline constexpr std::string_view diagnosticCpuPassBeginOverheadStatistic
        = "OpenMW diagnostic CPU pass begin callback overhead";
    inline constexpr std::string_view diagnosticCpuPassEndOverheadStatistic
        = "OpenMW diagnostic CPU pass end callback overhead";

    /// Return the versioned camera-pass identities emitted by performance diagnostics.
    std::span<const DiagnosticGpuPass> diagnosticGpuPasses();

    /// Enable asynchronous GPU statistics for one camera and retain it for diagnostic reporting.
    void enableDiagnosticGpuPassStatistics(osg::Camera& camera);

    /// Enable asynchronous GPU statistics for every camera below one scene-graph owner.
    void enableDiagnosticGpuPassStatistics(osg::Node& root);

    /// Enable or suspend camera and render-bin timing collection without detaching instrumentation.
    void setDiagnosticGpuPassStatisticsCollection(bool enabled);

    /// Wrap one render-bin prototype with a delayed GPU timer while preserving its draw behavior.
    void enableDiagnosticGpuRenderBinStatistics(
        osgUtil::RenderBin& prototype, int binNumber, std::string_view gpuStatistic, std::string_view cpuStatistic);

    /// Propagate diagnostic GPU collection from a parent render camera to a dependent camera.
    void inheritDiagnosticGpuPassStatistics(osg::Camera& target, const osg::Camera& source);

    /// Return live cameras registered for diagnostic GPU-pass reporting.
    std::vector<osg::ref_ptr<osg::Camera>> diagnosticGpuPassCameras();
}

#endif
