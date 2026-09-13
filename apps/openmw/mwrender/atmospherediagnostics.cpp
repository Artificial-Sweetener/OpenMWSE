#include "atmospherediagnostics.hpp"

#include "renderdiagnosticbundle.hpp"
#include "renderdiagnosticcapture.hpp"
#include "shadowmapdiagnostics.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/fx/stateupdater.hpp>

namespace MWRender
{
    void AtmosphereDiagnostics::requestCapture(std::filesystem::path outputDirectory, bool captureComparisons)
    {
        std::lock_guard lock(mRequestMutex);
        if (mCaptureStage != CaptureStage::Idle)
        {
            Log(Debug::Warning) << "Ignored atmosphere diagnostic request because a comparison is already in progress";
            return;
        }
        mRequestedOutputDirectory = std::move(outputDirectory);
        mCaptureStage = CaptureStage::NormalPending;
        mCaptureComparisons = captureComparisons;
    }

    bool AtmosphereDiagnostics::captureRequested() const
    {
        std::lock_guard lock(mRequestMutex);
        if (mSettleFramesRemaining != 0)
            return false;
        switch (mCaptureStage)
        {
            case CaptureStage::NormalPending:
            case CaptureStage::ShadowBypassPending:
            case CaptureStage::CloudSurfaceShadowBypassPending:
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
        if (mSettleFramesRemaining != 0)
        {
            --mSettleFramesRemaining;
            return ComparisonAction::None;
        }

        constexpr unsigned int comparisonSettleFrameCount = 3;
        if (mCaptureStage == CaptureStage::AwaitingShadowBypass)
        {
            mCaptureStage = CaptureStage::ShadowBypassPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::BypassShadows;
        }
        if (mCaptureStage == CaptureStage::AwaitingCloudSurfaceShadowBypass)
        {
            mCaptureStage = CaptureStage::CloudSurfaceShadowBypassPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::BypassCloudSurfaceShadows;
        }
        if (mCaptureStage == CaptureStage::AwaitingCoverageBypass)
        {
            mCaptureStage = CaptureStage::CoverageBypassPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::BypassCoveragePreservation;
        }
        if (mCaptureStage == CaptureStage::AwaitingFogBypass)
        {
            mCaptureStage = CaptureStage::FogBypassPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::BypassCompatibilityFog;
        }
        if (mCaptureStage == CaptureStage::AwaitingFogVisibility)
        {
            mCaptureStage = CaptureStage::FogVisibilityPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::ShowFogVisibility;
        }
        if (mCaptureStage == CaptureStage::AwaitingFogInScattering)
        {
            mCaptureStage = CaptureStage::FogInScatteringPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::ShowFogInScattering;
        }
        if (mCaptureStage == CaptureStage::AwaitingPreFogRadiance)
        {
            mCaptureStage = CaptureStage::PreFogRadiancePending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::ShowPreFogRadiance;
        }
        if (mCaptureStage == CaptureStage::AwaitingSampledSurfaceColor)
        {
            mCaptureStage = CaptureStage::SampledSurfaceColorPending;
            mSettleFramesRemaining = comparisonSettleFrameCount;
            return ComparisonAction::ShowSampledSurfaceColor;
        }
        if (mCaptureStage == CaptureStage::AwaitingNormalRestore)
        {
            mCaptureStage = CaptureStage::Idle;
            mRequestedOutputDirectory.reset();
            mSettleFramesRemaining = 0;
            return ComparisonAction::RestoreNormalRendering;
        }
        return ComparisonAction::None;
    }

    void AtmosphereDiagnostics::captureFrame(osg::RenderInfo& renderInfo, osg::Texture* sceneColor,
        osg::Texture* opaqueColor, osg::Texture* sceneDepth, osg::Texture* sceneNormals,
        osg::Texture* previousSceneDepth, osg::Texture2D* rawVolume, osg::Texture2D* rawVolumeMetadata,
        osg::Texture2D* reprojectedMetadata, osg::Texture2D* resolvedVolume, osg::Texture2D* volumeMetadata,
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
                || captureStage == CaptureStage::CloudSurfaceShadowBypassPending
                || captureStage == CaptureStage::CoverageBypassPending || captureStage == CaptureStage::FogBypassPending
                || captureStage == CaptureStage::FogVisibilityPending
                || captureStage == CaptureStage::FogInScatteringPending
                || captureStage == CaptureStage::PreFogRadiancePending
                || captureStage == CaptureStage::SampledSurfaceColorPending;
            if (!mRequestedOutputDirectory || !pendingCapture || mSettleFramesRemaining != 0)
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
                case CaptureStage::CloudSurfaceShadowBypassPending:
                    return "cloud-surface-shadows-bypassed";
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
            ShadowMapDiagnosticCapture shadowMaps;
            const auto appendColor = [&](osg::Texture* texture, std::string name) {
                if (auto image = capture.captureColorTexture(texture, std::move(name)))
                    images.emplace_back(std::move(*image));
            };
            const auto appendDepth = [&](osg::Texture* texture, std::string name) {
                if (auto image = capture.captureDepthTexture(texture, std::move(name)))
                    images.emplace_back(std::move(*image));
            };

            appendColor(sceneColor, "scene-color-before-post-processing");
            appendColor(opaqueColor, "scene-opaque-color");
            appendDepth(sceneDepth, "scene-opaque-depth");
            appendColor(sceneNormals, "scene-normals");
            appendDepth(previousSceneDepth, "previous-scene-opaque-depth");
            appendColor(rawVolume, "atmosphere-volume-checkerboard-raw");
            appendColor(rawVolumeMetadata, "atmosphere-volume-checkerboard-metadata-raw");
            appendColor(reprojectedMetadata, "atmosphere-volume-metadata-reprojected");
            if (resolvedVolume)
            {
                std::optional<int> shaderFrame;
                const auto& stateSets = renderInfo.getState()->getStateSetStack();
                for (auto it = stateSets.rbegin(); it != stateSets.rend() && !shaderFrame; ++it)
                    shaderFrame = Fx::StateUpdater::getPublishedFrameNumber(**it);
                if (!shaderFrame)
                    throw std::runtime_error("Atmosphere capture cannot identify the published shader frame");
                std::optional<SceneUtil::CloudLayerUniforms> cloudLayers;
                for (auto it = stateSets.rbegin(); it != stateSets.rend() && !cloudLayers; ++it)
                    cloudLayers = Fx::StateUpdater::getPublishedCloudLayers(**it);
                if (!cloudLayers)
                    throw std::runtime_error("Atmosphere capture cannot identify the published cloud models");
                RenderDiagnosticImage models{ "atmosphere-cloud-models-v2", 14, 3, 4, {} };
                for (const osg::Vec4f& parameters : *cloudLayers)
                    models.mPixels.insert(models.mPixels.end(), parameters.ptr(), parameters.ptr() + 4);
                images.emplace_back(std::move(models));
                std::optional<SceneUtil::AtmosphereTerrainGrid> terrain;
                for (auto it = stateSets.rbegin(); it != stateSets.rend() && !terrain; ++it)
                    terrain = Fx::StateUpdater::getPublishedAtmosphereTerrain(**it);
                if (!terrain)
                    throw std::runtime_error("Atmosphere capture cannot identify the published terrain field");
                RenderDiagnosticImage terrainField{ "atmosphere-cloud-terrain-v1",
                    static_cast<std::uint32_t>(SceneUtil::AtmosphereTerrainGrid::sPackedSize + 2), 1, 4, {} };
                terrainField.mPixels.insert(
                    terrainField.mPixels.end(), terrain->mOriginSpacing.ptr(), terrain->mOriginSpacing.ptr() + 4);
                terrainField.mPixels.insert(terrainField.mPixels.end(),
                    { terrain->mHeightBounds.x(), terrain->mHeightBounds.y(),
                        static_cast<float>(SceneUtil::AtmosphereTerrainGrid::sSide), 0.f });
                for (const osg::Vec4f& heights : terrain->mHeights)
                    terrainField.mPixels.insert(terrainField.mPixels.end(), heights.ptr(), heights.ptr() + 4);
                images.emplace_back(std::move(terrainField));
                const int phase = *shaderFrame % 4;
                images.push_back({ "atmosphere-checkerboard-layout", 1, 1, 4,
                    { static_cast<float>(phase), static_cast<float>(resolvedVolume->getTextureWidth()),
                        static_cast<float>(resolvedVolume->getTextureHeight()), 2.f } });
            }
            appendColor(resolvedVolume, "atmosphere-volume-resolved");
            appendColor(volumeMetadata, "atmosphere-volume-metadata");
            appendColor(shafts, "atmosphere-shafts");
            appendColor(previousResolvedVolume, "previous-atmosphere-volume-resolved");
            appendColor(previousVolumeMetadata, "previous-atmosphere-volume-metadata");
            if (captureStage == CaptureStage::NormalPending)
            {
                constexpr unsigned shadowMapDiagnosticMaximumDimension = 1024;
                shadowMaps = captureShadowMapDiagnostics(capture, renderInfo, shadowMapDiagnosticMaximumDimension);
                images.insert(images.end(), std::make_move_iterator(shadowMaps.mImages.begin()),
                    std::make_move_iterator(shadowMaps.mImages.end()));
            }
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
            if (!shadowMaps.mMetadata.empty())
            {
                std::filesystem::path metadataPath = bundlePath;
                metadataPath.replace_extension(".shadow.yaml");
                writeShadowMapDiagnosticMetadata(metadataPath, shadowMaps.mMetadata);
            }
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
                {
                    if (mCaptureComparisons)
                        mCaptureStage = CaptureStage::AwaitingShadowBypass;
                    else
                    {
                        mCaptureStage = CaptureStage::Idle;
                        mRequestedOutputDirectory.reset();
                    }
                }
                else if (captureStage == CaptureStage::ShadowBypassPending)
                    mCaptureStage = CaptureStage::AwaitingCloudSurfaceShadowBypass;
                else if (captureStage == CaptureStage::CloudSurfaceShadowBypassPending)
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
