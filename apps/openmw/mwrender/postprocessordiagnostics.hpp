#ifndef OPENMW_MWRENDER_POSTPROCESSORDIAGNOSTICS_H
#define OPENMW_MWRENDER_POSTPROCESSORDIAGNOSTICS_H

#include <cstdint>
#include <filesystem>
#include <memory>

namespace MWRender
{
    class AtmosphereDiagnostics;
    class RenderFrameDiagnostics;
    class RenderingManager;

    /// Own post-processing capture state and transient diagnostic render transitions.
    class PostProcessorDiagnostics
    {
    public:
        /// Create diagnostic capture owners connected to the renderer control boundary.
        explicit PostProcessorDiagnostics(RenderingManager& rendering);

        /// Return the atmosphere pass-capture owner used by frame canvases.
        const std::shared_ptr<AtmosphereDiagnostics>& atmosphereCapture() const;

        /// Return the exact final-frame capture owner used by frame canvases.
        const std::shared_ptr<RenderFrameDiagnostics>& renderFrameCapture() const;

        /// Request a one-shot production pass capture when the diagnostic environment is configured.
        void requestAtmosphereCapture();

        /// Request a production pass capture in an explicitly owned artifact directory.
        void requestAtmosphereCapture(const std::filesystem::path& outputDirectory, bool captureComparisons = true);

        /// Report whether the production multi-frame atmosphere capture owns transient render state.
        bool atmosphereCaptureInProgress() const;

        /// Request production composites beginning at one exact scheduled route frame.
        void requestRenderFrames(const std::filesystem::path& outputPath, std::uint32_t frameCount,
            bool nativeResolution, std::uint64_t firstFrameNumber);

        /// Report whether an exact final-composite sequence is being captured or serialized.
        bool renderFrameCaptureInProgress() const;

        /// Apply the next transient renderer state required by an atmosphere comparison capture.
        void advanceAtmosphereComparison();

    private:
        RenderingManager& mRendering;
        std::shared_ptr<AtmosphereDiagnostics> mAtmosphereCapture;
        std::shared_ptr<RenderFrameDiagnostics> mRenderFrameCapture;
    };
}

#endif
