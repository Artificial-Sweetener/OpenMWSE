#include "postprocessordiagnostics.hpp"

#include <cstdlib>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/shadow.hpp>

#include "atmospherediagnostics.hpp"
#include "renderframediagnostics.hpp"
#include "renderingmanager.hpp"

namespace MWRender
{
    PostProcessorDiagnostics::PostProcessorDiagnostics(RenderingManager& rendering)
        : mRendering(rendering)
        , mAtmosphereCapture(std::make_shared<AtmosphereDiagnostics>())
        , mRenderFrameCapture(std::make_shared<RenderFrameDiagnostics>())
    {
    }

    const std::shared_ptr<AtmosphereDiagnostics>& PostProcessorDiagnostics::atmosphereCapture() const
    {
        return mAtmosphereCapture;
    }

    const std::shared_ptr<RenderFrameDiagnostics>& PostProcessorDiagnostics::renderFrameCapture() const
    {
        return mRenderFrameCapture;
    }

    void PostProcessorDiagnostics::requestAtmosphereCapture()
    {
        const char* outputDirectory = std::getenv("OPENMW_ATMOSPHERE_DIAGNOSTICS_DIR");
        if (!outputDirectory || outputDirectory[0] == '\0')
            return;

        requestAtmosphereCapture(std::filesystem::path(outputDirectory));
    }

    void PostProcessorDiagnostics::requestAtmosphereCapture(
        const std::filesystem::path& outputDirectory, bool captureComparisons)
    {
        mAtmosphereCapture->requestCapture(outputDirectory, captureComparisons);
        Log(Debug::Info) << (captureComparisons
                ? "Production atmosphere shader-stage comparison requested for the next screenshot frames"
                : "Production atmosphere normal-frame capture requested for the next screenshot frame");
    }

    bool PostProcessorDiagnostics::atmosphereCaptureInProgress() const
    {
        return mAtmosphereCapture->captureInProgress();
    }

    void PostProcessorDiagnostics::requestRenderFrames(const std::filesystem::path& outputPath,
        std::uint32_t frameCount, bool nativeResolution, std::uint64_t firstFrameNumber)
    {
        mRenderFrameCapture->requestCapture(outputPath, frameCount, nativeResolution, firstFrameNumber);
        Log(Debug::Info) << "Exact production-frame diagnostic sequence requested at " << outputPath;
    }

    bool PostProcessorDiagnostics::renderFrameCaptureInProgress() const
    {
        return mRenderFrameCapture->captureInProgress();
    }

    void PostProcessorDiagnostics::advanceAtmosphereComparison()
    {
        switch (mAtmosphereCapture->advanceComparison())
        {
            case AtmosphereDiagnostics::ComparisonAction::BypassShadows:
                SceneUtil::ShadowManager::instance().setDiagnosticShadowRenderingEnabled(false);
                Log(Debug::Info) << "Bypassing shadows for the second production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::BypassCloudSurfaceShadows:
                SceneUtil::ShadowManager::instance().setDiagnosticShadowRenderingEnabled(true);
                mRendering.setAtmosphereDiagnosticMode(7);
                Log(Debug::Info) << "Bypassing cloud surface shadows for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::BypassCoveragePreservation:
                SceneUtil::ShadowManager::instance().setDiagnosticShadowRenderingEnabled(true);
                mRendering.setAtmosphereDiagnosticMode(1);
                Log(Debug::Info) << "Bypassing alpha mip-coverage preservation for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::BypassCompatibilityFog:
                mRendering.setAtmosphereDiagnosticMode(2);
                Log(Debug::Info) << "Bypassing compatibility fog for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::ShowFogVisibility:
                mRendering.setAtmosphereDiagnosticMode(3);
                Log(Debug::Info) << "Showing compatibility fog visibility for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::ShowFogInScattering:
                mRendering.setAtmosphereDiagnosticMode(4);
                Log(Debug::Info) << "Showing compatibility fog in-scattering for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::ShowPreFogRadiance:
                mRendering.setAtmosphereDiagnosticMode(5);
                Log(Debug::Info) << "Showing object pre-fog radiance for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::ShowSampledSurfaceColor:
                mRendering.setAtmosphereDiagnosticMode(6);
                Log(Debug::Info) << "Showing object sampled surface color for the production diagnostic frame";
                break;
            case AtmosphereDiagnostics::ComparisonAction::RestoreNormalRendering:
                SceneUtil::ShadowManager::instance().setDiagnosticShadowRenderingEnabled(true);
                mRendering.setAtmosphereDiagnosticMode(0);
                Log(Debug::Info) << "Restored normal rendering after production diagnostics";
                break;
            case AtmosphereDiagnostics::ComparisonAction::None:
                break;
        }
    }
}
