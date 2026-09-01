#ifndef OPENMW_MWRENDER_ATMOSPHEREDIAGNOSTICS_H
#define OPENMW_MWRENDER_ATMOSPHEREDIAGNOSTICS_H

#include <filesystem>
#include <mutex>
#include <optional>

#include <osg/ref_ptr>

namespace osg
{
    class RenderInfo;
    class State;
    class Texture;
    class Texture2D;
}

namespace MWRender
{
    /// Own one-shot requests for lossless production-frame atmosphere diagnostics.
    class AtmosphereDiagnostics
    {
    public:
        /// Describe a render-state transition required by the next comparison frame.
        enum class ComparisonAction
        {
            None,
            BypassShadows,
            BypassCloudSurfaceShadows,
            BypassCoveragePreservation,
            BypassCompatibilityFog,
            ShowFogVisibility,
            ShowFogInScattering,
            ShowPreFogRadiance,
            ShowSampledSurfaceColor,
            RestoreNormalRendering,
        };

        /// Schedule a production capture, optionally including every comparison route.
        void requestCapture(std::filesystem::path outputDirectory, bool captureComparisons = true);

        /// Report whether a capture request is waiting for the render thread.
        bool captureRequested() const;

        /// Report whether any stage of the current multi-frame comparison remains active.
        bool captureInProgress() const;

        /// Advance the multi-frame comparison and return the render-state transition to apply before culling.
        ComparisonAction advanceComparison();

        /// Consume a request and write the supplied production textures and final framebuffer losslessly.
        void captureFrame(osg::RenderInfo& renderInfo, osg::Texture* sceneColor, osg::Texture* sceneDepth,
            osg::Texture* sceneNormals, osg::Texture* previousSceneDepth, osg::Texture2D* rawVolume,
            osg::Texture2D* resolvedVolume, osg::Texture2D* volumeMetadata, osg::Texture2D* shafts,
            osg::Texture2D* previousResolvedVolume, osg::Texture2D* previousVolumeMetadata,
            unsigned int finalFramebuffer, int finalWidth, int finalHeight);

    private:
        /// Identify the frame currently owned by the render thread.
        enum class CaptureStage
        {
            Idle,
            NormalPending,
            AwaitingShadowBypass,
            ShadowBypassPending,
            AwaitingCloudSurfaceShadowBypass,
            CloudSurfaceShadowBypassPending,
            AwaitingCoverageBypass,
            CoverageBypassPending,
            AwaitingFogBypass,
            FogBypassPending,
            AwaitingFogVisibility,
            FogVisibilityPending,
            AwaitingFogInScattering,
            FogInScatteringPending,
            AwaitingPreFogRadiance,
            PreFogRadiancePending,
            AwaitingSampledSurfaceColor,
            SampledSurfaceColorPending,
            AwaitingNormalRestore,
        };

        mutable std::mutex mRequestMutex;
        std::optional<std::filesystem::path> mRequestedOutputDirectory;
        CaptureStage mCaptureStage = CaptureStage::Idle;
        unsigned int mSettleFramesRemaining = 0;
        bool mCaptureComparisons = true;
    };
}

#endif
