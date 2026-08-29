#include "atmospherediagnostics.hpp"

#include "renderdiagnosticbundle.hpp"
#include "renderdiagnosticcapture.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>

namespace MWRender
{
    void AtmosphereDiagnostics::requestCapture(std::filesystem::path outputDirectory)
    {
        std::lock_guard lock(mRequestMutex);
        if (mCaptureStage != CaptureStage::Idle)
        {
            Log(Debug::Warning) << "Ignored atmosphere diagnostic request because a comparison is already in progress";
            return;
        }
        mRequestedOutputDirectory = std::move(outputDirectory);
        mCaptureStage = CaptureStage::NormalPending;
    }

    bool AtmosphereDiagnostics::captureRequested() const
    {
        std::lock_guard lock(mRequestMutex);
        switch (mCaptureStage)
        {
            case CaptureStage::NormalPending:
            case CaptureStage::ShadowBypassPending:
            case CaptureStage::CoverageBypassPending:
            case CaptureStage::FogBypassPending:
            case CaptureStage::FogVisibilityPending:
            case CaptureStage::FogInScatteringPending:
            case CaptureStage::PreFogRadiancePending:
            case CaptureStage::SampledSurfaceColorPending:
                return true;
            default:
                return false;
        }
    }

    bool AtmosphereDiagnostics::captureInProgress() const
    {
        std::lock_guard lock(mRequestMutex);
        return mCaptureStage != CaptureStage::Idle;
    }

    AtmosphereDiagnostics::ComparisonAction AtmosphereDiagnostics::advanceComparison()
    {
        std::lock_guard lock(mRequestMutex);
        if (mCaptureStage == CaptureStage::AwaitingShadowBypass)
        {
            mCaptureStage = CaptureStage::ShadowBypassPending;
            return ComparisonAction::BypassShadows;
        }
        if (mCaptureStage == CaptureStage::AwaitingCoverageBypass)
        {
            mCaptureStage = CaptureStage::CoverageBypassPending;
            return ComparisonAction::BypassCoveragePreservation;
        }
        if (mCaptureStage == CaptureStage::AwaitingFogBypass)
        {
            mCaptureStage = CaptureStage::FogBypassPending;
            return ComparisonAction::BypassCompatibilityFog;
        }
        if (mCaptureStage == CaptureStage::AwaitingFogVisibility)
        {
            mCaptureStage = CaptureStage::FogVisibilityPending;
            return ComparisonAction::ShowFogVisibility;
        }
        if (mCaptureStage == CaptureStage::AwaitingFogInScattering)
        {
            mCaptureStage = CaptureStage::FogInScatteringPending;
            return ComparisonAction::ShowFogInScattering;
        }
        if (mCaptureStage == CaptureStage::AwaitingPreFogRadiance)
        {
            mCaptureStage = CaptureStage::PreFogRadiancePending;
            return ComparisonAction::ShowPreFogRadiance;
        }
        if (mCaptureStage == CaptureStage::AwaitingSampledSurfaceColor)
        {
            mCaptureStage = CaptureStage::SampledSurfaceColorPending;
            return ComparisonAction::ShowSampledSurfaceColor;
        }
        if (mCaptureStage == CaptureStage::AwaitingNormalRestore)
        {
            mCaptureStage = CaptureStage::Idle;
            mRequestedOutputDirectory.reset();
            return ComparisonAction::RestoreNormalRendering;
        }
        return ComparisonAction::None;
    }

    void AtmosphereDiagnostics::captureFrame(osg::RenderInfo& renderInfo, osg::Texture* sceneColor,
        osg::Texture* sceneDepth, osg::Texture* sceneNormals, osg::Texture* previousSceneDepth,
        osg::Texture2D* rawVolume, osg::Texture2D* resolvedVolume, osg::Texture2D* volumeMetadata,
        osg::Texture2D* shafts, osg::Texture2D* previousResolvedVolume, osg::Texture2D* previousVolumeMetadata,
        unsigned int finalFramebuffer, int finalWidth, int finalHeight)
    {
        std::filesystem::path outputDirectory;
        CaptureStage captureStage;
        {
            std::lock_guard lock(mRequestMutex);
            captureStage = mCaptureStage;
            const bool pendingCapture = captureStage == CaptureStage::NormalPending
                || captureStage == CaptureStage::ShadowBypassPending
                || captureStage == CaptureStage::CoverageBypassPending || captureStage == CaptureStage::FogBypassPending
                || captureStage == CaptureStage::FogVisibilityPending
                || captureStage == CaptureStage::FogInScatteringPending
                || captureStage == CaptureStage::PreFogRadiancePending
                || captureStage == CaptureStage::SampledSurfaceColorPending;
            if (!mRequestedOutputDirectory || !pendingCapture)
                return;
            outputDirectory = *mRequestedOutputDirectory;
        }

        const auto captureKind = [captureStage]() -> std::string_view {
            switch (captureStage)
            {
                case CaptureStage::NormalPending:
                    return "normal";
                case CaptureStage::ShadowBypassPending:
                    return "shadows-bypassed";
                case CaptureStage::CoverageBypassPending:
                    return "coverage-preservation-bypassed";
                case CaptureStage::FogBypassPending:
                    return "compatibility-fog-bypassed";
                case CaptureStage::FogVisibilityPending:
                    return "compatibility-fog-visibility";
                case CaptureStage::FogInScatteringPending:
                    return "compatibility-fog-in-scattering";
                case CaptureStage::PreFogRadiancePending:
                    return "object-pre-fog-radiance";
                case CaptureStage::SampledSurfaceColorPending:
                    return "object-sampled-surface-color";
                default:
                    return "invalid";
            }
        }();

        try
        {
            RenderDiagnosticCapture capture(renderInfo);
            std::vector<RenderDiagnosticImage> images;
            const auto appendColor = [&](osg::Texture* texture, std::string name) {
                if (auto image = capture.captureColorTexture(texture, std::move(name)))
                    images.emplace_back(std::move(*image));
            };
            const auto appendDepth = [&](osg::Texture* texture, std::string name) {
                if (auto image = capture.captureDepthTexture(texture, std::move(name)))
                    images.emplace_back(std::move(*image));
            };

            appendColor(sceneColor, "scene-color-before-post-processing");
            appendDepth(sceneDepth, "scene-opaque-depth");
            appendColor(sceneNormals, "scene-normals");
            appendDepth(previousSceneDepth, "previous-scene-opaque-depth");
            appendColor(rawVolume, "atmosphere-volume-raw");
            appendColor(resolvedVolume, "atmosphere-volume-resolved");
            appendColor(volumeMetadata, "atmosphere-volume-metadata");
            appendColor(shafts, "atmosphere-shafts");
            appendColor(previousResolvedVolume, "previous-atmosphere-volume-resolved");
            appendColor(previousVolumeMetadata, "previous-atmosphere-volume-metadata");
            images.emplace_back(
                capture.captureFramebuffer(finalFramebuffer, finalWidth, finalHeight, "final-composite"));

            const std::uint64_t frameNumber = renderInfo.getState()->getFrameStamp()->getFrameNumber();
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                       .count();
            std::filesystem::create_directories(outputDirectory);
            const std::filesystem::path bundlePath = outputDirectory
                / ("atmosphere-" + std::string(captureKind) + "-frame-" + std::to_string(frameNumber) + "-"
                    + std::to_string(timestamp) + ".omwdiag");
            writeRenderDiagnosticBundle(bundlePath, frameNumber, images);
            Log(Debug::Info) << "Captured production atmosphere diagnostics to " << bundlePath;
        }
        catch (const std::exception& exception)
        {
            Log(Debug::Error) << "Failed to capture production atmosphere diagnostics: " << exception.what();
        }

        {
            std::lock_guard lock(mRequestMutex);
            if (mCaptureStage == captureStage)
            {
                if (captureStage == CaptureStage::NormalPending)
                    mCaptureStage = CaptureStage::AwaitingShadowBypass;
                else if (captureStage == CaptureStage::ShadowBypassPending)
                    mCaptureStage = CaptureStage::AwaitingCoverageBypass;
                else if (captureStage == CaptureStage::CoverageBypassPending)
                    mCaptureStage = CaptureStage::AwaitingFogBypass;
                else if (captureStage == CaptureStage::FogBypassPending)
                    mCaptureStage = CaptureStage::AwaitingFogVisibility;
                else if (captureStage == CaptureStage::FogVisibilityPending)
                    mCaptureStage = CaptureStage::AwaitingFogInScattering;
                else if (captureStage == CaptureStage::FogInScatteringPending)
                    mCaptureStage = CaptureStage::AwaitingPreFogRadiance;
                else if (captureStage == CaptureStage::PreFogRadiancePending)
                    mCaptureStage = CaptureStage::AwaitingSampledSurfaceColor;
                else if (captureStage == CaptureStage::SampledSurfaceColorPending)
                    mCaptureStage = CaptureStage::AwaitingNormalRestore;
            }
        }
    }
}
