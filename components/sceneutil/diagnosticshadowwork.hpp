#ifndef OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICSHADOWWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICSHADOWWORK_H

#include <string_view>

#include <osg/Vec3d>
#include <osg/Vec4f>

namespace osgUtil
{
    class RenderBin;
}

namespace osg
{
    class Camera;
}

namespace SceneUtil
{
    /// Stores the post-sort render-leaf count for one shadow map and frame.
    inline constexpr std::string_view diagnosticShadowRenderLeafCountStatistic
        = "OpenMW diagnostic shadow render leaves";
    /// Stores the post-sort state-graph count for one shadow map and frame.
    inline constexpr std::string_view diagnosticShadowStateGraphCountStatistic
        = "OpenMW diagnostic shadow state graphs";
    /// Stores the absolute vertical component of the normalized directional shadow light.
    inline constexpr std::string_view diagnosticShadowLightVerticalMagnitudeStatistic
        = "OpenMW diagnostic shadow light vertical magnitude";
    /// Stores the scene-linear relative luminance of the directional shadow light.
    inline constexpr std::string_view diagnosticShadowLightLuminanceStatistic
        = "OpenMW diagnostic shadow light luminance";

    /// Retain the directional-light state that determines shadow-map projection and visible contribution.
    void setDiagnosticShadowLightState(
        osg::Camera& camera, const osg::Vec3d& lightDirection, const osg::Vec4f& diffuseColor);

    /// Associate one nested shadow camera with the frame currently being culled.
    void setDiagnosticShadowWorkFrame(osg::Camera& camera, unsigned frame);

    /// Records post-sort shadow submission counts when bounded diagnostics are active.
    class DiagnosticShadowWorkRecorder
    {
    public:
        /// Retain the shadow bin whose final submitted work will be recorded at scope exit.
        explicit DiagnosticShadowWorkRecorder(osgUtil::RenderBin& renderBin) noexcept;

        /// Publish the final render-leaf and state-graph counts for the current shadow frame.
        ~DiagnosticShadowWorkRecorder();

        DiagnosticShadowWorkRecorder(const DiagnosticShadowWorkRecorder&) = delete;
        DiagnosticShadowWorkRecorder& operator=(const DiagnosticShadowWorkRecorder&) = delete;

    private:
        osgUtil::RenderBin& mRenderBin;
    };
}

#endif
