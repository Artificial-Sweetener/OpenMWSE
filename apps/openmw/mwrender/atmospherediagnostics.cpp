#include "atmospherediagnostics.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/FrameBufferObject>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>

namespace MWRender
{
    namespace
    {
        /// Hold one named floating-point framebuffer attachment before bundle serialization.
        struct CapturedFloatImage
        {
            std::string mName;
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;
            std::uint32_t mChannels = 0;
            std::vector<float> mPixels;
        };

        /// Append a trivially copyable value to the versioned diagnostic stream.
        template <class T>
        void writeValue(std::ofstream& stream, const T& value)
        {
            stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
            if (!stream)
                throw std::runtime_error("failed writing atmosphere diagnostic value");
        }

        /// Append a length-prefixed UTF-8 label to the versioned diagnostic stream.
        void writeString(std::ofstream& stream, std::string_view value)
        {
            const auto size = static_cast<std::uint32_t>(value.size());
            writeValue(stream, size);
            stream.write(value.data(), static_cast<std::streamsize>(value.size()));
            if (!stream)
                throw std::runtime_error("failed writing atmosphere diagnostic label");
        }

        /// Read a two-dimensional color texture into scene-linear RGBA floats.
        std::optional<CapturedFloatImage> captureColorTexture(
            osg::State& state, osg::Texture* texture, std::string name)
        {
            auto* texture2D = dynamic_cast<osg::Texture2D*>(texture);
            if (!texture2D || texture2D->getTextureWidth() <= 0 || texture2D->getTextureHeight() <= 0)
                return std::nullopt;

            osg::ref_ptr<osg::FrameBufferObject> framebuffer = new osg::FrameBufferObject;
            framebuffer->setAttachment(
                osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(texture2D));
            framebuffer->apply(state, osg::FrameBufferObject::READ_FRAMEBUFFER);
            glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

            CapturedFloatImage image;
            image.mName = std::move(name);
            image.mWidth = static_cast<std::uint32_t>(texture2D->getTextureWidth());
            image.mHeight = static_cast<std::uint32_t>(texture2D->getTextureHeight());
            image.mChannels = 4;
            image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight * image.mChannels);
            glReadPixels(0, 0, static_cast<GLsizei>(image.mWidth), static_cast<GLsizei>(image.mHeight), GL_RGBA,
                GL_FLOAT, image.mPixels.data());
            return image;
        }

        /// Read a packed depth-stencil texture into normalized depth floats.
        std::optional<CapturedFloatImage> captureDepthTexture(
            osg::State& state, osg::Texture* texture, std::string name)
        {
            auto* texture2D = dynamic_cast<osg::Texture2D*>(texture);
            if (!texture2D || texture2D->getTextureWidth() <= 0 || texture2D->getTextureHeight() <= 0)
                return std::nullopt;

            osg::ref_ptr<osg::FrameBufferObject> framebuffer = new osg::FrameBufferObject;
            framebuffer->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER,
                osg::FrameBufferAttachment(texture2D));
            framebuffer->apply(state, osg::FrameBufferObject::READ_FRAMEBUFFER);
            glReadBuffer(GL_NONE);

            CapturedFloatImage image;
            image.mName = std::move(name);
            image.mWidth = static_cast<std::uint32_t>(texture2D->getTextureWidth());
            image.mHeight = static_cast<std::uint32_t>(texture2D->getTextureHeight());
            image.mChannels = 1;
            image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight);
            glReadPixels(0, 0, static_cast<GLsizei>(image.mWidth), static_cast<GLsizei>(image.mHeight),
                GL_DEPTH_COMPONENT, GL_FLOAT, image.mPixels.data());
            return image;
        }

        /// Read the post-processing destination before later GUI cameras draw over it.
        CapturedFloatImage captureFinalFramebuffer(
            osg::GLExtensions& extensions, unsigned int framebuffer, int width, int height)
        {
            extensions.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, framebuffer);
            glReadBuffer(framebuffer == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0_EXT);

            CapturedFloatImage image;
            image.mName = "final-composite";
            image.mWidth = static_cast<std::uint32_t>(width);
            image.mHeight = static_cast<std::uint32_t>(height);
            image.mChannels = 4;
            image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight * image.mChannels);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, image.mPixels.data());
            return image;
        }

        /// Persist all pass images in one deterministic, lossless, self-describing bundle.
        void writeCaptureBundle(
            const std::filesystem::path& path, std::uint64_t frameNumber, const std::vector<CapturedFloatImage>& images)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream)
                throw std::runtime_error("failed opening atmosphere diagnostic bundle");

            constexpr std::array<char, 8> magic{ 'O', 'M', 'W', 'A', 'T', 'M', 'D', '1' };
            stream.write(magic.data(), magic.size());
            writeValue(stream, std::uint32_t{ 1 });
            writeValue(stream, frameNumber);
            writeValue(stream, static_cast<std::uint32_t>(images.size()));
            for (const CapturedFloatImage& image : images)
            {
                writeString(stream, image.mName);
                writeValue(stream, image.mWidth);
                writeValue(stream, image.mHeight);
                writeValue(stream, image.mChannels);
                const std::uint64_t valueCount = image.mPixels.size();
                writeValue(stream, valueCount);
                stream.write(reinterpret_cast<const char*>(image.mPixels.data()),
                    static_cast<std::streamsize>(valueCount * sizeof(float)));
                if (!stream)
                    throw std::runtime_error("failed writing atmosphere diagnostic pixels");
            }
        }
    }

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

        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* extensions = state.get<osg::GLExtensions>();
        GLint previousReadFramebuffer = 0;
        GLint previousReadBuffer = GL_BACK;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &previousReadFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);

        try
        {
            std::vector<CapturedFloatImage> images;
            const auto appendColor = [&](osg::Texture* texture, std::string name) {
                if (auto image = captureColorTexture(state, texture, std::move(name)))
                    images.emplace_back(std::move(*image));
            };
            const auto appendDepth = [&](osg::Texture* texture, std::string name) {
                if (auto image = captureDepthTexture(state, texture, std::move(name)))
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
            images.emplace_back(captureFinalFramebuffer(*extensions, finalFramebuffer, finalWidth, finalHeight));

            const std::uint64_t frameNumber = state.getFrameStamp()->getFrameNumber();
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                       .count();
            std::filesystem::create_directories(outputDirectory);
            const std::filesystem::path bundlePath = outputDirectory
                / ("atmosphere-" + std::string(captureKind) + "-frame-" + std::to_string(frameNumber) + "-"
                    + std::to_string(timestamp) + ".omwdiag");
            writeCaptureBundle(bundlePath, frameNumber, images);
            Log(Debug::Info) << "Captured production atmosphere diagnostics to " << bundlePath;
        }
        catch (const std::exception& exception)
        {
            Log(Debug::Error) << "Failed to capture production atmosphere diagnostics: " << exception.what();
        }

        extensions->glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, previousReadFramebuffer);
        glReadBuffer(previousReadBuffer);

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
