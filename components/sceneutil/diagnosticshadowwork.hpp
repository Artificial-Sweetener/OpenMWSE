#ifndef OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICSHADOWWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICSHADOWWORK_H

#include <array>
#include <string_view>
#include <vector>

#include <osg/Matrixd>
#include <osg/Vec3d>
#include <osg/Vec4f>
#include <osg/ref_ptr>

namespace osgUtil
{
    class RenderBin;
}

namespace osg
{
    class Camera;
    class Texture2D;
}

namespace SceneUtil
{
    /// Describes one shadow map exactly as it was submitted to a graphics context.
    struct DiagnosticShadowMap
    {
        /// Identifies the frame that produced the map.
        unsigned mFrame = 0;
        /// Identifies the graphics context that owns the texture.
        unsigned mContext = 0;
        /// Identifies the logical shadow cascade.
        unsigned mCascade = 0;
        /// Retains the rendered depth texture until diagnostics consume it.
        osg::ref_ptr<osg::Texture2D> mTexture;
        /// Stores the light-view transform used to render the map.
        osg::Matrixd mViewMatrix;
        /// Stores the light-projection transform used to render the map.
        osg::Matrixd mProjectionMatrix;
        /// Stores the receiver crop as x, y, and z minimum/maximum pairs.
        std::array<double, 6> mReceiverCrop{ -1.0, 1.0, -1.0, 1.0, -1.0, 1.0 };
        /// Stores the camera-space near boundary assigned to the cascade.
        double mCascadeNear = 0.0;
        /// Stores the camera-space far boundary assigned to the cascade.
        double mCascadeFar = 0.0;
    };

    /// Store the dimensions of shadow textures submitted by completed shadow cameras.
    inline constexpr std::string_view diagnosticShadowTextureWidthStatistic = "OpenMW diagnostic shadow texture width";
    /// Store the rendered shadow texture height in pixels.
    inline constexpr std::string_view diagnosticShadowTextureHeightStatistic
        = "OpenMW diagnostic shadow texture height";

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
    /// Stores the receiver-away light-view boundary used for conservative caster culling.
    inline constexpr std::string_view diagnosticShadowCasterCullMinimumStatistic
        = "OpenMW diagnostic shadow caster cull minimum";
    /// Stores the light-view depth span of the receiver volume used for caster culling.
    inline constexpr std::string_view diagnosticShadowReceiverDepthSpanStatistic
        = "OpenMW diagnostic shadow receiver depth span";

    /// Retain the directional-light state that determines shadow-map projection and visible contribution.
    void setDiagnosticShadowLightState(
        osg::Camera& camera, const osg::Vec3d& lightDirection, const osg::Vec4f& diffuseColor);

    /// Attach frame-accurate shadow-map publication to one nested render camera.
    void enableDiagnosticShadowMapCapture(osg::Camera& camera, osg::Texture2D& texture);

    /// Retain the receiver crop and cascade interval used to build one shadow camera.
    void setDiagnosticShadowProjectionState(
        osg::Camera& camera, double cascadeNear, double cascadeFar, const std::array<double, 6>& receiverCrop);

    /// Retain the light-view receiver interval that determines conservative caster admission.
    void setDiagnosticShadowCasterCullState(osg::Camera& camera, double receiverMinimum, double receiverMaximum);

    /// Return shadow maps rendered for the requested graphics context and frame.
    std::vector<DiagnosticShadowMap> diagnosticShadowMaps(unsigned context, unsigned frame);

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
